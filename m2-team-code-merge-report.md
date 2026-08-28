# M2 팀원 코드 통합 보고서

## 1. 통합 목적

현재 `lsm-tree` 브랜치의 코드와 팀원이 작성한 M2 코드는 모두 ext4의 임의
쓰기를 ZNS 장치의 순차 쓰기로 변환하는 것을 목표로 한다. 두 구현을 함수
단위로 비교한 뒤, 기능과 안정성이 더 높은 부분을 선택해 하나의 코드로
통합했다.

통합 과정에서는 다음 원칙을 적용했다.

- 이미 정상 동작하던 LSM 구조와 M1/M2 기능은 유지한다.
- 하위 장치 쓰기가 실패해도 잘못된 매핑이 남지 않도록 한다.
- ext4가 사용하는 FLUSH, DISCARD, WRITE_ZEROES를 명확하게 처리한다.
- 추후 M3 GC에 활용할 수 있는 정보도 함께 수집한다.

## 2. 통합 전 코드 비교

### 기존 `lsm-tree` 코드의 장점

- 모든 I/O를 ordered workqueue에서 처리하여 ZNS 쓰기 순서를 보장한다.
- 하위 장치 쓰기가 성공한 뒤에만 LSM 매핑을 갱신한다.
- 4 KiB 단위 논리 블록 매핑을 사용한다.
- mutable MemTable과 immutable sorted run이 구현되어 있다.
- 여러 run의 최신 값을 sequence number로 선택한다.
- 4개의 같은 level run을 다음 level로 합치는 compaction이 구현되어 있다.
- FLUSH를 데이터 I/O와 같은 ordered queue에서 처리한다.
- DISCARD를 tombstone으로 기록하며, 이후 읽기는 0으로 반환한다.
- mutex 기반으로 I/O와 compaction을 안전하게 동기화한다.

### 팀원 코드의 장점

- `REQ_OP_WRITE_ZEROES`를 zero page 기반의 일반 WRITE로 변환한다.
- 덮어쓰기로 무효화된 물리 공간을 zone별 `invalid_sectors`로 집계한다.
- `logical_chunk`, `zone_chunk_off` 등 매핑 의미가 드러나는 이름을 사용한다.
- 4 KiB chunk 경계를 고려하여 READ/WRITE를 처리하려는 구조가 있다.

## 3. 팀원 코드에서 채택한 기능

### 3.1 WRITE_ZEROES 변환

팀원 코드의 WRITE_ZEROES 처리 아이디어를 기존 ordered workqueue 구조에 맞게
통합했다.

처리 과정은 다음과 같다.

```text
상위 WRITE_ZEROES 요청
  → ordered workqueue에 등록
  → active zone의 현재 write pointer 선택
  → zero page를 담은 일반 WRITE bio 생성
  → 하위 ZNS 장치에 순차 WRITE 제출
  → 쓰기 성공 확인
  → software write pointer 증가
  → LSM 매핑 등록
  → 원본 bio 완료
```

이 방식은 하위 ZNS 장치가 WRITE_ZEROES 명령을 직접 지원하지 않더라도 0을
정상적으로 기록할 수 있다. 또한 실제 쓰기가 성공한 후에만 매핑을 등록하므로
실패한 물리 주소가 최신 데이터로 조회되는 문제를 방지한다.

DM 장치가 WRITE_ZEROES를 받을 수 있도록 다음 설정도 추가했다.

- `num_write_zeroes_bios = 1`
- `max_write_zeroes_sectors = 8` (4 KiB)
- `max_write_zeroes_granularity = true`
- 자체 변환 기능을 DM core에 알리는 `.iterate_devices` callback

처음에는 `io_hints`에 최대 크기만 설정했지만 DM core가 하위 장치의
WRITE_ZEROES 지원 여부를 확인하면서 `write_zeroes_max_bytes`를 0으로
되돌렸다. 이 target은 요청을 일반 WRITE로 자체 변환하므로 하위 기능에
의존하지 않는 callback을 추가하여 문제를 해결했다.

### 3.2 invalid sector 집계

`zone_state`에 `invalid_sectors`를 추가했다. 동일한 논리 블록을 덮어쓰거나
discard tombstone을 기록할 때 기존 최신 매핑을 먼저 조회한다. 유효한 기존
데이터가 있으면 그 데이터가 위치한 zone의 `invalid_sectors`를 8 sector만큼
증가시킨다.

M1/M2에서는 이 값을 수집만 한다. 이후 M3에서는 전체 사용량 대비
`invalid_sectors` 비율이 높은 zone을 GC victim으로 선택하는 데 활용할 수
있다.

## 4. 기존 구현을 유지한 부분과 이유

### 쓰기 완료 후 매핑 갱신

팀원 코드는 하위 쓰기를 제출하기 전에 LSM 매핑과 software write pointer를
먼저 변경한다. 이 상태에서 하위 쓰기가 실패하면 실제 데이터가 없는 주소를
가리키는 매핑이 남을 수 있다. 따라서 기존 코드의 다음 순서를 유지했다.

```text
물리 위치 예약 → 하위 쓰기 → 성공 확인 → write pointer 및 매핑 갱신
```

### ordered workqueue

팀원 코드는 일부 동기 I/O를 Device Mapper의 `.map()` 함수에서 직접 실행한다.
현재 코드는 `.map()`에서는 작업을 queue에 넣고 즉시 반환하며, 실제 I/O는
ordered worker에서 수행한다. 이 구조가 `.map()` 경로의 장시간 block을 피하고
동시에 들어온 쓰기의 물리 순서를 보장하므로 유지했다.

### 완성된 LSM run과 compaction

팀원 코드의 SSTable은 flush 시 로그만 출력하는 stub이다. 현재 코드는
MemTable이 임계값에 도달하면 immutable run으로 회전하며, 같은 level의 run
4개를 다음 level로 compaction한다. 따라서 기존 LSM 구현을 유지했다.

### DISCARD tombstone

팀원 코드는 DISCARD 요청을 성공으로 완료하지만 기존 매핑을 무효화하지 않는다.
현재 구현은 tombstone을 최신 매핑으로 기록하고 이후 read에서 zero-fill하므로
현재 방식을 유지했다.

### zone 초기화

팀원 코드는 zone 0을 항상 active zone으로 선택한다. 현재 구현은 보고된 zone
상태와 write pointer를 확인하고 실제로 쓸 수 있는 zone을 찾으므로 기존 방식을
유지했다.

## 5. 변수명 선택 기준

기존 LSM 코드 전체를 불필요하게 변경하면 회귀 가능성이 커지므로 의미가 이미
명확한 이름은 유지했다.

- `logical_sector`: 4 KiB 정렬된 상위 논리 주소
- `zone_idx`: 데이터가 기록된 물리 zone 번호
- `zone_offset`: zone 시작점 기준 sector offset
- `physical_sector`: 하위 장치에 제출할 최종 sector 주소
- `invalid_sectors`: 덮어쓰기나 discard로 더 이상 최신 데이터가 아닌 공간

팀원 코드의 `logical_chunk`도 의미가 좋지만, 현재 구현은 sector 단위 Device
Mapper API와 직접 연결되므로 `logical_sector`를 유지했다.

## 6. 테스트 보강

`scripts/test-m2.sh`에 다음 검사를 추가했다.

1. `discard_max_bytes`가 4096 이상인지 확인한다.
2. discard한 논리 블록을 direct I/O로 읽어 모두 0인지 확인한다.
3. `write_zeroes_max_bytes`가 4096 이상인지 확인한다.
4. 임의 데이터 4 KiB를 기록한다.
5. `blkdiscard --zeroout`으로 WRITE_ZEROES를 발생시킨다.
6. direct I/O로 다시 읽어 모든 바이트가 0인지 비교한다.
7. ext4 생성, mount, 10 MiB 기록, unmount/remount 후 MD5를 비교한다.

전체 검증 명령은 다음과 같다.

```bash
sudo bash scripts/nullblk-up.sh
cd src && make clean && make && cd ..
sudo bash scripts/test-m1.sh
sudo bash scripts/test-m2.sh
sudo bash scripts/test-lsm.sh
```

통합 완료 후 실제 Ubuntu VM의 zoned `null_blk` 환경에서 M1, M2 ext4
round-trip, WRITE_ZEROES, discard tombstone, MemTable 회전 및 LSM compaction
검사가 모두 성공했다.

## 7. 최종 통합 결과

최종 코드는 기존 구현의 I/O 순서 보장과 완성된 in-memory LSM 구조를 유지하면서
팀원 코드의 WRITE_ZEROES 오류 해결 방법과 GC용 invalid sector 집계를 포함한다.

```text
기존 코드에서 유지
  ordered I/O, 성공 후 매핑, 4 KiB 분할, FLUSH, tombstone,
  MemTable 회전, immutable run, sequence 기반 조회, compaction

팀원 코드에서 채택
  WRITE_ZEROES → 일반 순차 WRITE 변환, zone별 invalid sector 집계

추가 보완
  DM queue 기능 공개, iterate_devices 처리, WRITE_ZEROES 전용 회귀 테스트
```

## 8. 팀원 또는 다른 AI에게 전달할 요약 프롬프트

> 기존 `lsm-tree` 구현과 팀원의 M2 구현을 비교하여 통합했다. 기존 코드의
> ordered workqueue, 하위 쓰기 성공 후 매핑 갱신, 4 KiB 매핑, FLUSH barrier,
> discard tombstone, immutable run 및 compaction은 유지했다. 팀원 코드에서는
> WRITE_ZEROES를 zero page 기반 일반 WRITE로 변환하는 아이디어와 zone별
> invalid sector 집계를 채택했다. WRITE_ZEROES도 ordered worker에서 처리하며,
> 하위 순차 WRITE가 성공한 뒤 software write pointer와 LSM 매핑을 갱신한다.
> DM queue에는 4 KiB WRITE_ZEROES 지원을 공개했고, 하위 장치가 해당 명령을
> 지원하지 않아도 자체 변환할 수 있도록 iterate_devices 처리도 추가했다.
> `test-m2.sh`는 discard tombstone, WRITE_ZEROES 변환, ext4 unmount/remount 후
> MD5 일치를 검증한다. M1, M2, LSM 테스트는 실제 null_blk 환경에서 모두
> 통과했다. 앞으로 코드를 수정할 때 이 I/O 순서와 LSM 최신 값 선택 규칙을
> 깨뜨리지 않아야 한다.

