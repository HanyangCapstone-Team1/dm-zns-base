# dm-zns-base

ZNS SSD 위에서 ext4 같은 일반 파일시스템을 사용하기 위한 졸업프로젝트이다. Linux Device Mapper 타깃에서 **임의 쓰기를 zone 내부 순차 쓰기로 변환**하고, 논리 주소와 실제 저장 위치를 메모리 LSM 구조로 관리한다.

프로젝트 주제: 엣지 컴퓨팅을 위한 동적 플랫폼(Apache Kafka)과 ZNS(Zoned Namespace) SSD 호환을 위한 리눅스 커널 블록 계층 개발.

## 구현 내용

- **M1:** 4 KiB 임의 쓰기 변환과 일반 블록 장치 인터페이스
- **M2:** ext4 생성·마운트·재마운트, 읽기/쓰기, 논리 DISCARD와 WRITE_ZEROES
- **LSM:** RB-tree Memtable, 1024개 항목 단위 회전, 같은 레벨 run 4개 병합
- **M3:** active zone pool, reserve zone, greedy / cost-benefit GC, compaction 연계 GC
- **GC 개선:** 선제 GC에서 무효 데이터 비율이 기본 20% 이상인 후보만 선택해 불필요한 복사를 줄임

현재 타깃 이름은 `zns-m1`, 버전은 `0.8.1`이다. 이름은 M1부터 유지하지만 M2·LSM·M3 구현도 포함한다. Kafka와의 최종 통합 성능평가, 매핑 영속화와 크래시 복구는 포함하지 않는다.

## 코드 구조와 처리 흐름

| 경로 | 설명 |
|---|---|
| `src/dm-zns-base.c` | DM 타깃, LSM 매핑, compaction, zone GC |
| `src/Makefile` | 실행 중인 커널 헤더를 이용한 모듈 빌드 |
| `scripts/test-m1.sh` | 임의 쓰기와 기본 블록 기능 검사 |
| `scripts/test-m2.sh` | ext4와 데이터 무결성 검사 |
| `scripts/test-lsm.sh` | M2 실행 중 Memtable 회전·compaction 검사 |
| `scripts/test-m3.sh` | 채우기·덮어쓰기·GC·CRC 검증 |
| `scripts/test-kernel-log.sh`, `scripts/capture-kernel.py` | 실행 구간의 커널 로그 수집과 누락 검사 |
| `scripts/nullblk-*.sh` | 로컬 zoned null_blk 생성·제거 |
| `docs/` | 초기 환경 구성, 마일스톤, 참고자료 |

1. `zns_m1_map()`이 4 KiB 정렬을 확인하고 ordered I/O workqueue에 작업을 등록한다.
2. 쓰기는 active zone의 write pointer에 기록한 뒤 매핑을 갱신한다. 읽기는 최신 sequence의 매핑을 찾아 실제 위치에서 읽는다. 미기록·삭제 블록은 0을 반환한다.
3. Memtable이 차면 메모리 run으로 전환한다. `compact_runs()`는 중복 키의 최신 버전을 남기며 같은 레벨 run을 다음 레벨로 병합한다.
4. `run_zone_gc()`는 공간 부족 시 reserve로 유효 데이터를 옮기고 victim을 reset한다. `run_compaction_gc()`는 compaction 이후 조건이 맞을 때 active zone의 여유 공간으로 유효 데이터를 옮겨 free zone을 확보한다.

모든 run은 RAM에 있으며 디스크 SSTable이나 WAL은 없다. 메타데이터 병합과 물리 데이터 GC는 서로 다른 작업이다. I/O와 물리 GC는 같은 ordered queue에서 실행하므로 GC 중 전경 I/O가 지연될 수 있다. `docs/`에는 초기 안내도 포함하므로 현재 실행 순서는 이 README와 실제 소스를 기준으로 확인한다.

## 환경과 빌드

검증 환경: Ubuntu 22.04.5 LTS, Linux `5.15.0-186-generic`, fio `3.28`, FEMU ZNS 32 GiB(zone 2 GiB × 16개). Linux 5.18 전후 bio API 호환 분기가 있으며, 다른 커널에서는 별도 확인이 필요하다.

```bash
git clone https://github.com/HanyangCapstone-Team1/dm-zns-base.git
cd dm-zns-base
git switch main
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) fio dmsetup util-linux e2fsprogs python3
make -C src
```

결과는 `src/dm-zns-base.ko`이다. FEMU 서버에서는 VM 부팅 후 **게스트 안에서** 빌드·모듈 적재·테스트를 수행한다.

## 자동 테스트

**테스트는 지정한 하위 장치의 모든 zone을 reset하고 데이터를 덮어쓴다. 실험 전용 장치를 사용한다.** 마운트된 파일시스템, 실행 중인 fio, 기존 DM 장치를 먼저 정상 종료하고 한 번에 한 테스트씩 실행한다. 아래 `/dev/nvme0n1`은 실제 실험 장치 이름에 맞게 지정한다.

```bash
lsblk -o NAME,SIZE,TYPE,MOUNTPOINTS
cat /sys/block/nvme0n1/queue/zoned   # host-managed 확인
sudo blkzone report /dev/nvme0n1 | head

sudo env UNDERLYING=/dev/nvme0n1 bash scripts/test-m1.sh
sudo env UNDERLYING=/dev/nvme0n1 bash scripts/test-m2.sh
sudo env UNDERLYING=/dev/nvme0n1 bash scripts/test-lsm.sh
sudo env UNDERLYING=/dev/nvme0n1 GC_POLICY=cost-benefit ACTIVE_ZONE_POOL_SIZE=2 bash scripts/test-m3.sh
# 앞 테스트 종료 후 greedy 정책을 별도로 실행
sudo env UNDERLYING=/dev/nvme0n1 GC_POLICY=greedy ACTIVE_ZONE_POOL_SIZE=2 bash scripts/test-m3.sh
```

M3는 논리 용량의 80%를 채우고 같은 범위에 장치 용량의 1.2배를 덮어쓴 뒤, 최대 64 MiB의 새 데이터에 CRC 검증을 수행한다. 전체 기존 데이터의 CRC 검증은 아니다. 용량과 GC 복사량에 따라 오래 걸리므로 `Starting 1 process`만으로 정지를 판단하지 말고 다른 터미널의 `sudo dmesg -w`로 GC 진행을 확인한다. 테스트는 종료 시 생성한 DM 장치와 모듈을 정리한다.

로컬 기능 검사는 메모리 기반 `null_blk`로도 가능하다.

```bash
sudo bash scripts/nullblk-up.sh
sudo env UNDERLYING=/dev/nullb0 bash scripts/test-m1.sh
sudo env UNDERLYING=/dev/nullb0 bash scripts/test-lsm.sh
# 필요하면 같은 장치로 test-m3.sh 실행
sudo bash scripts/nullblk-down.sh
```

`null_blk`는 실제 SSD 성능을 재현하지 않으므로 기능 검사용으로 사용한다.

## 로그와 옵션

```bash
mkdir -p results
set -o pipefail
sudo env UNDERLYING=/dev/nvme0n1 GC_POLICY=cost-benefit \
  bash scripts/test-m3.sh 2>&1 | tee "results/m3-$(date +%Y%m%d-%H%M%S).log"
```

LSM·M3의 연속 커널 로그는 기본적으로 `results/kernel-logs/run.*/`에 저장된다. 로그 누락이나 수집기 오류도 실패 처리한다. GC 로그의 `moved`, `reclaimed` 단위는 512-byte sector이고 시작·완료 시각으로 GC 소요 시간을 계산할 수 있다.

| 모듈 파라미터 | 기본값 | 설명 |
|---|---|---|
| `gc_policy` | `cost-benefit` | `greedy` 또는 `cost-benefit` |
| `active_zone_pool_size` | `2` | 요청 active zone 수. 장치 제한에 따라 줄어들 수 있음 |
| `compaction_gc_min_invalid_percent` | `20` | 선제 GC 후보의 최소 무효 비율(0~100). `0`이면 필터 해제 |

옵션은 모듈 적재 시 지정한다. 예: `sudo insmod src/dm-zns-base.ko gc_policy=cost-benefit active_zone_pool_size=2 compaction_gc_min_invalid_percent=20`. 공간 부족 시 reserve GC에는 이 비율 필터를 적용하지 않는다. `test-m3.sh`는 자체적으로 모듈을 재적재하며 환경변수로는 `GC_POLICY`와 `ACTIVE_ZONE_POOL_SIZE`를 전달한다.

## 한계

매핑은 메모리에만 있으므로 **DM 장치 제거, 모듈 재적재, 재부팅 이후 기존 데이터를 복구할 수 없다.** M2 재마운트 검증은 같은 DM 장치가 유지되는 동안의 검사다. FLUSH도 매핑 영속화를 보장하지 않는다. reserve와 GC 작업 공간이 필요하므로 노출된 논리 용량 전체를 유효 데이터로 채울 수 있다는 보장도 없다.

GPL-2.0. 소스의 SPDX와 모듈 라이선스 선언을 따른다.
