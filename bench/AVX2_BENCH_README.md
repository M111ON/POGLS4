# POGLS V4 — AVX2 Benchmark
## วิธีรันบน Windows 10 x64

---

### ไฟล์ที่ต้องการ
```
pogls_v4_avx2_bench.c   ← ไฟล์เดียว ไม่ต้องการ header อื่น
```

---

## Option A — MSYS2 + MinGW-w64 (แนะนำ — ฟรี ติดตั้งง่าย)

### 1. ติดตั้ง MSYS2
ดาวน์โหลดจาก https://www.msys2.org/ → รัน installer

### 2. เปิด "MSYS2 UCRT64" terminal แล้วรัน
```bash
pacman -S mingw-w64-ucrt-x86_64-gcc
```

### 3. Compile
```bash
gcc -O3 -mavx2 -march=native -funroll-loops \
    pogls_v4_avx2_bench.c -o avx2_bench.exe
```

### 4. Run
```bash
./avx2_bench.exe
```

---

## Option B — WSL2 (Ubuntu) บน Windows 10

### 1. เปิด WSL2 terminal
```bash
sudo apt install gcc -y
```

### 2. Compile
```bash
gcc -O3 -mavx2 -march=native -funroll-loops -lpthread \
    pogls_v4_avx2_bench.c -o avx2_bench
```

### 3. Run
```bash
./avx2_bench
```
> หมายเหตุ: WSL2 runs on the Windows kernel — ตัวเลขที่ได้จะใกล้เคียง native มาก
> G4400 ไม่มี Hyper-V overhead บน WSL2 เพราะ single-core ทดสอบ

---

## Option C — MSVC (Visual Studio 2019/2022)

### 1. เปิด "x64 Native Tools Command Prompt"
(Start → Visual Studio → x64 Native Tools Command Prompt for VS 2022)

### 2. Compile
```cmd
cl /O2 /arch:AVX2 /fp:fast pogls_v4_avx2_bench.c
```

### 3. Run
```cmd
pogls_v4_avx2_bench.exe
```

---

## ผลที่คาดบน G4400 @ 3.3GHz (1 core, no HT)

| Tier | คาดประมาณ | เหตุผล |
|------|-----------|--------|
| S0 Scalar baseline | ~280–320 M/s | G4400 IPC ต่ำกว่า Xeon Colab ~3× |
| S1 Scalar + prefetch | ~300–350 M/s | prefetch ช่วยน้อยบน single core |
| A1 AVX2 PHI scatter ×8 | ~500–700 M/s | 8-wide SIMD บน Skylake-era AVX2 |
| A2 AVX2 + unit circle | ~350–500 M/s | unit circle เพิ่ม 64-bit mul lanes |
| A3 AVX2 hybrid | ~200–280 M/s | scalar conf pass bottleneck |
| A4 AVX2 + prefetch | ~360–520 M/s | prefetch ช่วย A2 path |
| T2 AVX2 + 2-shard | ~480–700 M/s | G4400 มี 2 thread (1 core HT) |

**Production estimate (with disk, 70% cache hit):**
- S0 baseline × 0.30 = ~84–96 M/s
- **ห่างจาก spec 13.6 M/s ประมาณ 6–7× (ปลอดภัยมาก)**

---

## อ่านผล

```
S0: Scalar baseline         xxx.x M/s  (1.00x)  <- base
S1: Scalar + prefetch       xxx.x M/s  (+x.xxf)
A1: AVX2 PHI scatter x8     xxx.x M/s  (+x.xxf)   <- pure vector width gain
A2: AVX2 scatter + inv      xxx.x M/s  (+x.xxf)   <- เพิ่ม unit circle
A3: AVX2 hybrid             xxx.x M/s  (+x.xxf)   <- full pipeline
A4: AVX2 + prefetch         xxx.x M/s  (+x.xxf)
T2: AVX2+2shard (wall)      xxx.x M/s  (+x.xxf)   <- 2-thread wall time
```

**สิ่งที่ AVX2 vectorize:**
- PHI scatter: `aa = (addr × PHI_UP) >> 20` — ×8 addrs per instruction
- Unit circle: `aa² + bb² >> 41` — ×8 parallel 64-bit mul
- PHI delta: `|addr - prev| ∈ [PHI_DOWN±TOL]` — ×8 range check

**สิ่งที่ต้อง scalar (chain-dependent):**
- `d2 = |a - prev_a|` — prev_a ขึ้นกับ iteration ก่อนหน้า
- `rel = a ^ prev_mask` — prev_mask update ทุก step
- `conf` final merge — logic รวม fp/fl/fg/inv

---

## G4400 CPU Check (ยืนยัน AVX2 รองรับ)

รันใน PowerShell:
```powershell
wmic cpu get Name,NumberOfCores,NumberOfLogicalProcessors
```

G4400 = Skylake, AVX2 รองรับ ✓

หรือดาวน์โหลด CPU-Z → แท็บ "Instructions" ควรเห็น AVX2

---

## Troubleshooting

| ปัญหา | วิธีแก้ |
|-------|---------|
| `Illegal instruction` | CPU ไม่รองรับ AVX2 — ลอง `-mavx` แทน |
| `pthread` error บน MSVC | ไม่ต้อง — Windows ใช้ `CreateThread` ใน code แล้ว |
| ตัวเลขต่ำกว่าคาด | ปิด background apps, อย่ารันใน VM ที่ throttle CPU |
| `gcc not found` MSYS2 | เช็คว่าใช้ UCRT64 terminal ไม่ใช่ MSYS terminal |
