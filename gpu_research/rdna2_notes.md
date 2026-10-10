# RDNA 2 ISA: извлечения для работы движка

Источник: `"RDNA 2" Instruction Set Architecture: Reference Guide`, AMD, 291 стр.,
ModDate 2020-12-01 (https://gpuopen.com/download/rdna2-shader-instruction-set-architecture.pdf).
Извлечено программно (pymupdf) §§3.11, 7.5, 8.1 (.2/.5/.7/.8), 8.2 (вводная). Полный текст
не читался; ниже — только сверенное с выводом парсера.

## §3.11 Memory Violations (стр. 22/283) — ФАКТ

Нарушение репортится из: LDS alignment error; memory read/write/atomic alignment error;
flat-доступ вне aperture; **запись в read-only surface**; GDS alignment/range error;
GWS abort. НЕ репортится для instruction/scalar-data доступов. `BUFFER_LOAD_*_TO_LDS`
с out-of-range LDS-адресом НЕ даёт violation (маскирует EXEC-биты).
При нарушении LDS/кэш возвращают MEM_VIOL → sticky-бит TRAPSTS.mem_viol.
**Нарушения фатальны**: прыжок в trap handler, иначе interrupt + halt.
**Нарушения НЕ precise**: PC сохранённого состояния не связан с виноватой инструкцией.

Применение к нам: (1) выравнивание обязательно везде (наш heap-фикс 32 и проверки
alignment — в правильном направлении); (2) write-to-read-only — аппаратно ловится,
наш учёт readOnly/write в дескрипторах обязателен; (3) не-precision означает: даже на
консоли вылет указывал бы мимо виноватой инструкции — диагностика только по данным.

## §7.5 Alignment and Bounds Checking, скаляры (стр. 54) — ФАКТ

- SDST чётный для fetch 2 dword / кратен 4 для больших, иначе invalid data.
- SBASE чётный для S_BUFFER_LOAD, иначе берётся SGPR0.
- OFFSET без ограничений выравнивания.
- **Out-of-range memory address (clamped): операция НЕ выполняется для Dword вне диапазона.**

## §8.1 Buffer Instructions (стр. 55–65) — ФАКТ

- VMEM: MTBUF (typed, формат в инструкции) / MUBUF (untyped, формат в константе) /
  MIMG (image + семплер S#) / FLAT / GLOBAL / SCRATCH. `TBUFFER_*` — также vertex fetch.
- V# = 128 бит в 4 SGPR (Table 37):
  - 47:0 — Base address (48 бит, байтовый адрес);
  - 61:48 — Stride, 14 бит (0–16383);
  - 62 — Cache swizzle; 63 — Swizzle enable;
  - 95:64 — Num_records (в единицах stride если stride ≥ 1, иначе в байтах);
  - 98:96/101:99/104:102/107:105 — Dst_sel xyzw;
  - 114:108 — Format (7 бит);
  - 118:117 — Index stride; 119 — Add tid enable; 120 — Resource Level (=1);
  - 125:124 — OOB_SELECT; 127:126 — Type (== 0 для буфера);
  - **ресурс из всех нулей = unbound (возвращает 0,0,0,0)**.
- §8.1.7 Alignment: formatted ops — строго по element_size; неформатированные
  dword+ — младшие 2 бита игнорируются (принудительно dword); режим — через
  SH_MEM_CONFIG.alignment_mode (DWORD / DWORD_STRICT / STRICT / UNALIGNED).
- GLC/DLC/SLC (§8.1.10): GLC=1 load — мимо L0 с инвалидацией; stores — всегда
  Miss-Evict из L0, комбинируются по wavefront; атомики без return при GLC=0.

## Сверка с нашим декодером V# (ФАКТ совпадения)

| Поле ISA | Наш декодер | Статус |
|---|---|---|
| Base 47:0 | `fields[0] \| ((fields[1] & 0xffff) << 32)` | CONFIRMED |
| Stride 61:48 (14 бит) | `(fields[1] >> 16) & 0x3fff` | CONFIRMED |
| Num_records 95:64 | `fields[2]` | CONFIRMED |
| Format 114:108 (7 бит) | `(fields[3] >> 12) & 0x7f` | CONFIRMED |
| Type 127:126 == 0 | `Require(Type() == 0)` | CONFIRMED |
| All-zero = unbound | `NullVertexDescriptor` (все 4 слова 0) | CONFIRMED |

## Что НЕ извлечено (и не утверждается)

- Уравнение simplified buffer addressing (§8.1.1) — рисунок, текстом не извлекалось: UNKNOWN.
- MIMG поля дескрипторов (§8.2.1+): не читались — UNKNOWN.
- Flat/Global/Scratch детали (§9): не читались — UNKNOWN.
- GNM-пакеты/регистры/вызовы: в ISA их нет по построению — UNKNOWN (как и было).
