// Package k8s — 제어부가 데이터플레인 Deployment 를 조종하는 계층.
//
// 왜 API 를 거쳐야 하는가: 개수·리소스 변경은 **어떤 경우에도 데이터플레인
// 재시작을 수반한다**. 세 겹의 제약이 겹쳐 있다.
//
//  1. Pod 의 spec.containers 는 불변 — 컨테이너 개수 변경 = Pod 재생성
//  2. in-place resize 는 static CPU manager + Guaranteed 조합에서 Infeasible.
//     hugepages 와 확장 리소스는 애초에 resize 대상도 아니다.
//  3. DPDK 가 런타임 lcore 변경을 지원하지 않는다 (rte_eal_init 이 고정)
//
// 그래서 목표는 "무중단"이 아니라 **재시작 범위를 데이터플레인으로만 한정**하는
// 것이고, 제어부가 별도 파드에 있어야 그게 성립한다.
package k8s

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"

	corev1 "k8s.io/api/core/v1"
	metav1 "k8s.io/apimachinery/pkg/apis/meta/v1"
	"k8s.io/apimachinery/pkg/types"
	"k8s.io/client-go/kubernetes"
	"k8s.io/client-go/rest"
	"k8s.io/client-go/tools/clientcmd"
)

// DataplaneContainer 는 리소스를 조정할 컨테이너 이름이다.
// 사이드카(agent)는 공유 풀에 남아야 하므로 절대 건드리지 않는다.
const DataplaneContainer = "dataplane"

type Client struct {
	cs         kubernetes.Interface
	namespace  string
	deployment string
	pfResource corev1.ResourceName
	labelSel   string
	log        *slog.Logger
}

type Config struct {
	Namespace  string
	Deployment string
	PFResource string // 예: "mir.io/dpdk_pf"
	LabelSel   string // 예: "app=mir-dataplane"
	Log        *slog.Logger
}

// New 는 in-cluster 설정을 우선 쓰고, 없으면 ~/.kube/config 로 폴백한다
// (로컬에서 제어부만 띄워 붙여 볼 때를 위한 통로).
func New(cfg Config) (*Client, error) {
	restCfg, err := rest.InClusterConfig()
	if err != nil {
		kubeconfig := os.Getenv("KUBECONFIG")
		if kubeconfig == "" {
			home, _ := os.UserHomeDir()
			kubeconfig = filepath.Join(home, ".kube", "config")
		}
		restCfg, err = clientcmd.BuildConfigFromFlags("", kubeconfig)
		if err != nil {
			return nil, fmt.Errorf("k8s 설정을 찾지 못했다 (in-cluster 도 kubeconfig 도 없음): %w", err)
		}
		cfg.Log.Warn("in-cluster 설정 없음 — kubeconfig 사용", "path", kubeconfig)
	}

	cs, err := kubernetes.NewForConfig(restCfg)
	if err != nil {
		return nil, fmt.Errorf("클라이언트 생성 실패: %w", err)
	}

	return &Client{
		cs:         cs,
		namespace:  cfg.Namespace,
		deployment: cfg.Deployment,
		pfResource: corev1.ResourceName(cfg.PFResource),
		labelSel:   cfg.LabelSel,
		log:        cfg.Log,
	}, nil
}

func (c *Client) Clientset() kubernetes.Interface { return c.cs }
func (c *Client) Namespace() string               { return c.namespace }

// ───────────────────────────────────────────────────────────
// 용량 조회
// ───────────────────────────────────────────────────────────

type NodeCapacity struct {
	Name           string `json:"name"`
	AllocatablePF  int64  `json:"allocatablePf"`
	AllocatableCPU string `json:"allocatableCpu"`
	Hugepages1Gi   string `json:"hugepages1Gi"`
	Hugepages2Mi   string `json:"hugepages2Mi"`
}

type Capacity struct {
	Nodes []NodeCapacity `json:"nodes"`

	// MaxDataplanes 는 데이터플레인 파드 개수의 상한이다.
	// 파드 하나가 PF 하나를 점유하므로 클러스터 전체 PF 개수와 같다.
	MaxDataplanes int32 `json:"maxDataplanes"`

	CurrentReplicas int32 `json:"currentReplicas"`
}

func (c *Client) Capacity(ctx context.Context) (*Capacity, error) {
	nodes, err := c.cs.CoreV1().Nodes().List(ctx, metav1.ListOptions{})
	if err != nil {
		return nil, fmt.Errorf("노드 조회 실패: %w", err)
	}

	result := &Capacity{}
	for i := range nodes.Items {
		n := &nodes.Items[i]
		alloc := n.Status.Allocatable

		nc := NodeCapacity{Name: n.Name}
		if q, ok := alloc[c.pfResource]; ok {
			nc.AllocatablePF = q.Value()
		}
		if q, ok := alloc[corev1.ResourceCPU]; ok {
			nc.AllocatableCPU = q.String()
		}
		if q, ok := alloc["hugepages-1Gi"]; ok {
			nc.Hugepages1Gi = q.String()
		}
		if q, ok := alloc["hugepages-2Mi"]; ok {
			nc.Hugepages2Mi = q.String()
		}

		result.MaxDataplanes += int32(nc.AllocatablePF)
		result.Nodes = append(result.Nodes, nc)
	}

	replicas, err := c.CurrentReplicas(ctx)
	if err != nil {
		return nil, err
	}
	result.CurrentReplicas = replicas
	return result, nil
}

func (c *Client) CurrentReplicas(ctx context.Context) (int32, error) {
	s, err := c.cs.AppsV1().Deployments(c.namespace).
		GetScale(ctx, c.deployment, metav1.GetOptions{})
	if err != nil {
		return 0, fmt.Errorf("scale 조회 실패: %w", err)
	}
	return s.Spec.Replicas, nil
}

// ───────────────────────────────────────────────────────────
// 개수 조정
// ───────────────────────────────────────────────────────────

// ErrOverCapacity 는 요청한 개수가 PF 개수를 넘을 때 반환한다.
//
// 이 검증을 API 단계에서 하지 않으면 초과분이 조용히 Pending 에 쌓이고,
// 사용자는 "왜 안 뜨지"를 파드 이벤트를 뒤져서 알아내야 한다.
type ErrOverCapacity struct {
	Requested int32
	Max       int32
}

func (e *ErrOverCapacity) Error() string {
	return fmt.Sprintf("데이터플레인 %d개를 요청했으나 사용 가능한 NIC PF 는 %d개다", e.Requested, e.Max)
}

func (c *Client) Scale(ctx context.Context, replicas int32) error {
	if replicas < 0 {
		return fmt.Errorf("replicas 는 음수일 수 없다: %d", replicas)
	}

	capacity, err := c.Capacity(ctx)
	if err != nil {
		return err
	}
	if replicas > capacity.MaxDataplanes {
		return &ErrOverCapacity{Requested: replicas, Max: capacity.MaxDataplanes}
	}

	patch, err := json.Marshal(map[string]any{
		"spec": map[string]any{"replicas": replicas},
	})
	if err != nil {
		return err
	}

	// scale 서브리소스를 패치한다. Deployment 본체가 아니라 여기를 건드리면
	// RBAC 을 deployments/scale 로 좁게 유지할 수 있다.
	_, err = c.cs.AppsV1().Deployments(c.namespace).Patch(
		ctx, c.deployment, types.MergePatchType, patch, metav1.PatchOptions{}, "scale")
	if err != nil {
		return fmt.Errorf("scale 패치 실패: %w", err)
	}

	c.log.Info("데이터플레인 개수 변경", "replicas", replicas)
	return nil
}

// ───────────────────────────────────────────────────────────
// 리소스 조정
// ───────────────────────────────────────────────────────────

// ResourceSpec — 빈 문자열인 항목은 건드리지 않는다.
type ResourceSpec struct {
	CPU       string `json:"cpu,omitempty"`
	Memory    string `json:"memory,omitempty"`
	Hugepages string `json:"hugepages,omitempty"`

	// HugepageSize 는 "1Gi" 또는 "2Mi". 비우면 1Gi.
	// 클라우드에서는 커널 cmdline 을 못 건드려 2Mi 가 현실적이다.
	HugepageSize string `json:"hugepageSize,omitempty"`
}

// SetResources 는 pod template 의 dataplane 컨테이너 리소스를 바꾼다.
// **데이터플레인 파드가 롤링 재생성된다** — 제어부와 GUI 는 영향받지 않는다.
//
// requests 와 limits 를 항상 같은 값으로 쓴다. Guaranteed QoS 의 조건이고,
// 그래야 CPU Manager static 이 배타 코어를 준다.
func (c *Client) SetResources(ctx context.Context, spec ResourceSpec) error {
	res := map[string]string{}
	if spec.CPU != "" {
		res["cpu"] = spec.CPU
	}
	if spec.Memory != "" {
		res["memory"] = spec.Memory
	}
	if spec.Hugepages != "" {
		size := spec.HugepageSize
		if size == "" {
			size = "1Gi"
		}
		res["hugepages-"+size] = spec.Hugepages
	}
	if len(res) == 0 {
		return fmt.Errorf("변경할 항목이 없다")
	}

	// strategic merge patch 는 containers 를 name 으로 병합하므로
	// dataplane 컨테이너만 정확히 겨냥할 수 있다 (agent 는 그대로 둔다).
	patch, err := json.Marshal(map[string]any{
		"spec": map[string]any{
			"template": map[string]any{
				"spec": map[string]any{
					"containers": []any{
						map[string]any{
							"name": DataplaneContainer,
							"resources": map[string]any{
								"requests": res,
								"limits":   res,
							},
						},
					},
				},
			},
		},
	})
	if err != nil {
		return err
	}

	_, err = c.cs.AppsV1().Deployments(c.namespace).Patch(
		ctx, c.deployment, types.StrategicMergePatchType, patch, metav1.PatchOptions{})
	if err != nil {
		return fmt.Errorf("리소스 패치 실패: %w", err)
	}

	c.log.Info("데이터플레인 리소스 변경 — 롤링 재생성 시작", "resources", res)
	return nil
}

// ───────────────────────────────────────────────────────────
// 파드 조회 / 재시작
// ───────────────────────────────────────────────────────────

type DataplanePod struct {
	Name      string            `json:"name"`
	Phase     string            `json:"phase"`
	Node      string            `json:"node"`
	PodIP     string            `json:"podIp"`
	Ready     bool              `json:"ready"`
	Restarts  int32             `json:"restarts"`
	Resources map[string]string `json:"resources"`
	Message   string            `json:"message,omitempty"`
}

func (c *Client) ListDataplanes(ctx context.Context) ([]DataplanePod, error) {
	pods, err := c.cs.CoreV1().Pods(c.namespace).
		List(ctx, metav1.ListOptions{LabelSelector: c.labelSel})
	if err != nil {
		return nil, fmt.Errorf("파드 조회 실패: %w", err)
	}

	out := make([]DataplanePod, 0, len(pods.Items))
	for i := range pods.Items {
		p := &pods.Items[i]

		d := DataplanePod{
			Name:      p.Name,
			Phase:     string(p.Status.Phase),
			Node:      p.Spec.NodeName,
			PodIP:     p.Status.PodIP,
			Resources: map[string]string{},
		}

		for j := range p.Spec.Containers {
			ctr := &p.Spec.Containers[j]
			if ctr.Name != DataplaneContainer {
				continue
			}
			for name, q := range ctr.Resources.Requests {
				d.Resources[string(name)] = q.String()
			}
		}

		ready := len(p.Status.ContainerStatuses) > 0
		for j := range p.Status.ContainerStatuses {
			cs := &p.Status.ContainerStatuses[j]
			if !cs.Ready {
				ready = false
			}
			d.Restarts += cs.RestartCount
		}
		d.Ready = ready

		// Pending 의 이유를 그대로 노출한다. PF 부족으로 스케줄 못 하는 경우가
		// 가장 흔한데, 그 사유가 여기 담긴다.
		for j := range p.Status.Conditions {
			cond := &p.Status.Conditions[j]
			if cond.Type == corev1.PodScheduled && cond.Status != corev1.ConditionTrue {
				d.Message = cond.Message
			}
		}

		out = append(out, d)
	}
	return out, nil
}

// RestartPod 는 파드를 삭제한다. Deployment 가 즉시 새로 만든다.
func (c *Client) RestartPod(ctx context.Context, name string) error {
	if err := c.cs.CoreV1().Pods(c.namespace).
		Delete(ctx, name, metav1.DeleteOptions{}); err != nil {
		return fmt.Errorf("파드 삭제 실패: %w", err)
	}
	c.log.Info("데이터플레인 파드 재시작", "pod", name)
	return nil
}
