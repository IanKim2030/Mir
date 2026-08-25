project('mir-dataplane', 'c',
  version: '0.1.0',
  license: 'Apache-2.0',
  meson_version: '>=0.61',
  default_options: [
    'c_std=c11',
    'warning_level=2',
    'optimization=2',
  ])

# ── 의존성 ────────────────────────────────────────────────────
# 순수 C 만 쓴다. gRPC(C++ 런타임)를 들이지 않는 것이 이 프로젝트의 설계 결정이라
# libstdc++·abseil·BoringSSL 이 여기 나타나면 안 된다.
dpdk       = dependency('libdpdk',       version: '>=22.11')
protobuf_c = dependency('libprotobuf-c', version: '>=1.3')
threads    = dependency('threads')

# mbedTLS (Phase 5b) — 순수 C TLS. pkg-config 파일이 없어 라이브러리를 직접
# 찾는다. 세 라이브러리 모두 필요하다: tls(ssl) → x509 → crypto.
cc         = meson.get_compiler('c')
mbedtls    = cc.find_library('mbedtls')
mbedx509   = cc.find_library('mbedx509')
mbedcrypto = cc.find_library('mbedcrypto')

# ── 빌드 플래그 ───────────────────────────────────────────────
#
# _GNU_SOURCE 는 여기서 주지 않는다. 각 .c 가 첫 줄에 스스로 정의하고 있어
# 중복 정의 경고가 매 빌드마다 나왔다. 파일이 자기 요구사항을 직접 선언하는
# 쪽을 남긴다 — 빌드 시스템 없이 개별 파일만 컴파일해 검증할 수 있다.
add_project_arguments('-DMIR_VERSION="' + meson.project_version() + '"', language: 'c')

# -march=native 는 금지다. 빌드 머신과 실행 머신의 CPU 가 다르면 SIGILL 로
# 죽는데, 컨테이너 이미지는 정의상 다른 머신에서 돌 수 있다.
# 마이크로아키텍처 튜닝은 Phase 7(100G 튜닝)에서 이 옵션으로 승격한다.
march = get_option('march')
if march != ''
  add_project_arguments('-march=' + march, language: 'c')
endif

# ── protobuf-c 코드 생성 ──────────────────────────────────────
# proto 를 평평하게(하위 디렉터리 없이) 둔 이유가 여기 있다. protoc-c 는
# --c_out 아래에 패키지 디렉터리 계층을 만드는데, meson 의 custom_target 은
# output 에 하위 경로를 쓸 수 없다.
protoc_c  = find_program('protoc-c')
proto_dir = meson.current_source_dir() / '..' / 'proto'

pb_c = custom_target('dataplane-pb-c',
  input:   proto_dir / 'dataplane.proto',
  output: ['dataplane.pb-c.c', 'dataplane.pb-c.h'],
  command: [protoc_c, '--proto_path=' + proto_dir, '--c_out=@OUTDIR@', '@INPUT@'])

# ── 실행 파일 ─────────────────────────────────────────────────
executable('mir-dataplane',
  sources: [
    'src/main.c',
    'src/eal_args.c',
    'src/ipc_server.c',
    'src/port.c',
    'src/tx_hello.c',
    'src/pktbuild.c',
    'src/tx_engine.c',
    'src/rx_engine.c',
    'src/events.c',
    'src/session.c',
    'src/pcap_reader.c',
    'src/replay.c',
    'src/tls.c',
    pb_c,
  ],
  include_directories: include_directories('src'),
  dependencies: [dpdk, protobuf_c, threads, mbedtls, mbedx509, mbedcrypto],
  install: true)
