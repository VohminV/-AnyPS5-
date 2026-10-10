# Открытые вопросы и нужные материалы

## A. Неизвестно (требует закрытый SDK или поведенческих тестов)

1. Точные значения `PrimitiveType` GNM и какой регистр включает primitive restart
   (влияет на трактовку 0xFFFF в индексных буферах). Статус: UNKNOWN — не додумывать.
2. Кодировки дескрипторов ресурсов/текстур/вершинных атрибутов (поля, флаги, Type).
3. Семантика конкретных PM4-пакетов и порядок их исполнения.
4. Реальное поведение PS5 при OOB vertex fetch (fault vs мусор) — определяет, обязан ли
   эмулятор падать там, где падает сейчас.
5. Какие NID-функции реально дергает игра на проблемном участке (нужна NID-трасса,
   отдельная инженерная задача, не исследование).

## B. Нужные материалы (легальные пути получения)

| # | Материал | Как получить (без нарушения лицензий) | Статус |
|---|---|---|---|
| 1 | Vulkan SDK (Windows): слои, `vulkaninfo`, `vkconfig`, spirv-инструменты | https://vulkan.lunarg.com/sdk/home — скачать и поставить вручную | ⬇️ не ставился (исследованию не требовался) |
| 2 | RenderDoc + доки | https://renderdoc.org/ — скачать вручную; доки онлайн | ⬇️ не ставился |
| 3 | Nsight Graphics (для RTX 3050) | Сайт NVIDIA, бесплатная регистрация | ⬇️ не ставился |
| 4 | AMD RDNA ISA (в т.ч. RDNA2 — поколение PS5 GPU) | Хаб: https://rocm.docs.amd.com/en/latest/reference/gpu-arch/rdna.html; RDNA2 PDF: https://gpuopen.com/download/rdna2-shader-instruction-set-architecture.pdf (ФАКТ: 291 стр., AMD, 2020-12-01 — проверено gateway-запросом; зеркало TechPowerUp существует, но отдаёт бот-проверку). Выбор RDNA2 ISA как первичного референса кодировок НЕЗАВИСИМО подтверждён KytyPS5 (Developer Information). Табличные характеристики RDNA2 (wavefront 32, LDS 128 KiB, GFXIP 10.3) — из ROCm gpu-arch-specs (прочитана) | ✅ ссылки и сам PDF проверены; ⚠️ текст PDF не извлекался |
| 5 | PS5 SDK / AGC headers | Только под NDA Sony; локальный каталог пуст | ❌ недоступно — работать через clean-room наблюдения + тесты |
| 6 | Sync-validation whitepaper (LunarG PDF) | Ссылка из гайда (зафиксирована в sources.md) | 📖 ссылка есть, текст не читался |

## C. Следующие технические шаги (по ценности)

1. **Поставить LunarG SDK** → `vulkaninfo` (расширить дамп уже снятого driver-уровня:
   фичи/лимиты зафиксированы выше) → прогнать движок под `VK_LAYER_KHRONOS_validation` +
   sync validation → собрать и прогнать 8 демо из `demos/` (матрица в `demos/RESULTS.md`).
2. **RenderDoc-захват** проблемного кадра (с учётом BDA caveat) — сверить барьеры/ресурсы.
3. **`spirv-val`** по генерируемым SPIR-V модулям (debug-сборка).
4. **Прочитать RDNA2 ISA PDF** (Doc 70648, https://docs.amd.com/v/u/en-US/rdna2-shader-instruction-set-architecture):
   wave-модели, SGPR/VGPR, memory hierarchy — для сверки декодера шейдеров (НЕ для GNM API).
5. **Дочитать**: Synchronization Examples + whitepaper; `pipeline_cache` и
   `descriptor_management` семплы; Memory Model spec
   (https://docs.vulkan.org/spec/latest/chapters/memory_model.html — ссылка есть, текст не читался).
6. Поведенческие тесты для строк таблицы со статусом ❌ (по одному факту за раз).

## D. Зафиксированные ограничения исследования

- Сеть частично недоступна/медленна (таймауты websearch, docs.renderdoc.org) — часть
  вторичных источников не открылась; первичные (Khronos) прочитаны.
- Исполняемый код не запускался, SDK не ставился, игра не трогалась — всё выше
  соответствует миссии «только исследование».
