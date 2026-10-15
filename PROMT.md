# 🚀 FPS BOOST: PS5 → PC Relinker
### Конкретные техники повышения FPS, а не инфраструктура

> Фокус: **что реально даёт кадры**. Каждой технике — оценка прироста, код, метрика для замера.

---

## 📊 Карта прироста FPS (от большего к меньшему)

| Техника | Прирост FPS | Сложность | Где применять |
|---|---|---|---|
| **1. Occupancy / VGPR-оптимизация** | **+30-80%** | средняя | Шейдеры |
| **2. Async Compute overlap** | **+15-40%** | высокая | Frame pipeline |
| **3. Early-Z / Hi-Z + Mesh Shaders** | **+20-50%** | средняя | Geometry |
| **4. Bindless + Descriptor Buffer** | **+5-15%** | низкая | Дескрипторы |
| **5. FSR 3 / DLSS 3 Frame Gen** | **+60-150%** | низкая | Post-process |
| **6. Driver-level hacks** | **+5-20%** | нулевая | ENV vars |
| **7. Wave64→32 правильный фолбэк** | **+10-25%** на NVIDIA | средняя | Шейдеры |
| **8. Memory bandwidth (LDS, кэш)** | **+10-30%** | высокая | Compute |

---

## 1. 🏆 OCCUPANCY — главный рычаг (+30-80% FPS)

**Суть:** чем больше вейвов одновременно крутится на CU, тем лучше GPU прячет latency памяти. PS5-шейдеры часто жрут 60-80 VGPR → occupancy 25% → GPU простаивает.

### 1.1. Замер текущего состояния

```bash
# Linux/AMD — увидеть регистры и occupancy
RADV_PERFTEST=aco_print_stats ./ps5_relinker 2>&1 | grep -E "VGPR|SGPR|occupancy|LDS"

# Windows/AMD — через Radeon Developer Panel (RDP) или GPU-Util
# Windows/NVIDIA — через Nsight Graphics → Frame Profiler → Occupancy
```

**Целевые цифры:**
- VGPR ≤ 32 на лан (RDNA: 256 VGPR / CU / 4 SIMD = 64 на лан при wave64)
- Occupancy ≥ 50% (идеал 75%+)
- LDS ≤ 32 KB на workgroup

### 1.2. Техники снижения VGPR

```hlsl
// ❌ ПЛОХО: всё в VGPR
cbuffer Constants { float4x4 mvp; float3 lightDir; int quality; };
// mvp = 16 VGPR, lightDir = 3 VGPR, quality = 1 VGPR = 20 VGPR на лан
// НО mvp одинаков для всех ланов в вейве! Это SGPR-кандидат.

// ✅ ХОРОШО: specialization constants + scalar loads
[[vk::constant_id(0)]] const int QUALITY = 2;     // -> 0 VGPR, скаляр
[[vk::constant_id(1)]] const int USE_SHADOWS = 1; // -> 0 VGPR

// Матрицу — через push constant с scalar load
struct Push { float4x4 mvp; }; // компилятор сам загрузит в SGPR
[[vk::push_constant]] Push pc;

// Цикл с константной границей -> разворачивается
for (int i = 0; i < QUALITY; ++i) { ... }
```

### 1.3. Принудительная скаляризация через `spirv-opt`

```bash
# Эти проходы двигают uniform-данные в SGPR
spirv-opt -O \
  --scalar-replacement \
  --simplify-instructions \
  --ccp \
  --eliminate-dead-code-aggressive \
  input.spv -o output.spv
```

### 1.4. Ограничение регистров через `dxc`

```bash
# Жёсткий лимит VGPR (AMD RDNA)
dxc -O3 -spirv -fvk-max-vgpr-count=32 shader.hlsl
# Компилятор будет spill'ить в LDS/память, но occupancy вырастет
# Проверяйте: иногда spill дороже, чем потеря occupancy
```

**Метрика:** до/после замерить `aco_print_stats | grep VGPR`. Цель: -30% VGPR = +20-40% FPS.

---

## 2. ⚡ ASYNC COMPUTE OVERLAP (+15-40% FPS)

**Суть:** PS5 использует аппаратные асинхронные compute-очереди. На PC надо эмулировать через **несколько очередей Vulkan**.

### 2.1. Создание отдельной compute-очереди

```cpp
// Ищем очередь с VK_QUEUE_COMPUTE_BIT, но БЕЗ VK_QUEUE_GRAPHICS_BIT
// (такая очередь реально параллельна графической)
uint32_t graphicsFamily = findQueue(GRAPHICS | COMPUTE);
uint32_t asyncComputeFamily = findQueue(COMPUTE); // без GRAPHICS!

float priorities[] = { 1.0f, 0.5f };
VkDeviceQueueCreateInfo queues[] = {
    { .sType = ..., .queueFamilyIndex = graphicsFamily, .queueCount = 1, .pQueuePriorities = &priorities[0] },
    { .sType = ..., .queueFamilyIndex = asyncComputeFamily, .queueCount = 1, .pQueuePriorities = &priorities[1] },
};

// Семaфоры для синхронизации между очередями
VkSemaphore graphicsToCompute, computeToGraphics;
```

### 2.2. Паттерн overlap: G-buffer + Culling + Post-process параллельно

```cpp
// Frame pipeline с overlap
void RenderFrame() {
    // 1. Graphics queue: G-buffer pass
    vkCmdBeginRenderPass(graphicsCmd, &gbufferPass, ...);
    DrawScene(graphicsCmd);
    vkCmdEndRenderPass(graphicsCmd);
    
    // 2. Signal semaphore -> compute queue может стартовать
    VkSemaphoreSubmitInfo signalInfo = { .semaphore = graphicsToCompute, .stageMask = COLOR_ATTACHMENT_OUTPUT };
    VkSemaphoreSubmitInfo waitInfo   = { .semaphore = graphicsToCompute, .stageMask = COMPUTE_SHADER };
    vkQueueSubmit2(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    
    // 3. Compute queue: culling + lighting (ПАРАЛЛЕЛЬНО с следующей сценой!)
    vkQueueSubmit2(computeQueue, 1, &computeSubmitInfo, graphicsToCompute);
    
    // 4. Graphics queue: post-process (ждёт compute через другой семафор)
    vkQueueSubmit2(graphicsQueue, 1, &postSubmitInfo, computeToGraphics);
}
```

**Что параллелить (типичный PS5-пайплайн):**
- G-buffer pass ↔ Frustum/Hi-Z culling
- Forward lighting ↔ Particle simulation
- Shadow map ↔ Animation/skinning compute
- Post-process (tonemap/FSR) ↔ Audio/physics (на CPU, но разгружает GPU-очередь)

**Метрика:** RenderDoc → GPU timeline. Если compute-queue простаивает, пока graphics работает → overlap не работает. Цель: обе очереди активны 80%+ времени.

---

## 3. 🎯 EARLY-Z + MESH SHADERS (+20-50% FPS)

**Суть:** PS5 Geometry Engine = mesh shaders. На PC это `VK_EXT_mesh_shader`. Если нет mesh shader — fallback на compute+culling.

### 3.1. Mesh shader с primitive culling (эмуляция PS5 GE)

```hlsl
[numthreads(32, 1, 1)]
[outputTopology("triangle_cw")]
void MS_Main(uint tid : SV_GroupIndex,
             out vertices MSOut verts[64],
             out indices uint3 prims[124])
{
    SetMeshOutputCounts(0, 0); // начинаем с 0
    
    uint primCount = 0;
    uint vertCount = 0;
    
    // PS5-подобный culling: отбрасываем backface + small triangles
    for (uint i = 0; i < 128; ++i) {
        Triangle t = FetchTriangle(tid * 128 + i);
        
        // Backface culling
        float3 n = normalize(cross(t.e1, t.e2));
        if (dot(n, viewDir) < 0) continue;
        
        // Small-triangle culling (< 2 пикселя)
        float area = ScreenSpaceArea(t);
        if (area < 2.0) continue;
        
        // Frustum culling
        if (!FrustumTest(t)) continue;
        
        // Emit
        uint baseV = vertCount;
        verts[vertCount++] = MakeVert(t.v0);
        verts[vertCount++] = MakeVert(t.v1);
        verts[vertCount++] = MakeVert(t.v2);
        prims[primCount++] = uint3(baseV, baseV+1, baseV+2);
    }
    
    SetMeshOutputCounts(vertCount, primCount);
}
```

**Прирост:** на сценах с высоким overdraw (листва, волосы, частицы) — **+30-50%**. На простых сценах — 0.

### 3.2. Hi-Z occlusion culling (compute pre-pass)

```hlsl
// Compute shader: строит Hi-Z buffer, потом читает его для culling
// Запускается на async compute queue (overlap с предыдущим кадром!)
[numthreads(8, 8, 1)]
void HiZ_Build(uint3 tid : SV_DispatchThreadID) {
    // Минимум глубины из 2x2 блока предыдущего уровня
    float d0 = depth[tid.xy * 2 + uint2(0,0)];
    float d1 = depth[tid.xy * 2 + uint2(1,0)];
    float d2 = depth[tid.xy * 2 + uint2(0,1)];
    float d3 = depth[tid.xy * 2 + uint2(1,1)];
    hiz[tid.xy] = min(min(d0, d1), min(d2, d3));
}
```

---

## 4. 🔗 BINDLESS + DESCRIPTOR BUFFER (+5-15% FPS)

**Суть:** PS5 flat address space → bindless. Но bindless сам по себе не даёт FPS. Даёт — **descriptor buffer** (`VK_EXT_descriptor_buffer`).

### 4.1. Descriptor Buffer вместо Descriptor Sets

```cpp
// Старый путь (descriptor sets): 1000 обновлений в кадр = дорого
vkUpdateDescriptorSets(...); // CPU-bound

// Новый путь (descriptor buffer): заполняем буфер один раз
VkDescriptorBufferBindingInfoEXT bindInfo = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
    .address = descriptorBufferDeviceAddress,
    .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT,
};
vkCmdBindDescriptorBuffersEXT(cmd, 1, &bindInfo);

// В шейдере — прямой доступ по адресу
// Никаких vkUpdateDescriptorSets в горячем пути!
```

**Прирост:** на сценах с 10k+ объектов — **+10-15%**. На простых — 0.

### 4.2. Push descriptors для динамических данных

```cpp
// Вместо UBO для 1-2 структур — push descriptor (один вызов)
VkWriteDescriptorSet write = { ... };
vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &write);
```

---

## 5. 🖼️ FSR 3 / DLSS 3 FRAME GEN (+60-150% FPS)

**Суть:** PS5-игры часто 30-60 FPS. На PC можно поднять до 120+ через генерацию кадров.

### 5.1. FSR 3 integration (бесплатно, открытый код)

```cpp
// FFX FSR 3: Upscale + Frame Generation
FfxFsr3Context fsr3;
fsr3ContextCreate(&fsr3, &desc);

// Каждый кадр:
FfxFsr3DispatchDescription dispatch = {
    .commandList = cmd,
    .color = &currentColorResource,
    .depth = &depthResource,
    .motionVectors = &mvResource,
    .output = &upscaledOutput,
    .frameTimeDelta = deltaTime,
    .renderSize = { 1920, 1080 },    // внутренний рендер
    .displaySize = { 3840, 2160 },   // выход (4K)
    .enableAutoExposure = true,
};
fsr3ContextDispatch(&fsr3, &dispatch);

// Frame Generation включается отдельно (даёт +60-100% FPS)
FfxFsr3DispatchDescription fgDispatch = dispatch;
fgDispatch.flags = FFX_FSR3_DISPATCH_ENABLE_FRAME_GENERATION;
```

**Прирост:** 1080p → 4K с Frame Gen = **+150% FPS** визуально.

### 5.2. DLSS 3 (NVIDIA only, проприетарно)

```cpp
// Аналогично FSR, но через NVNGX API
NVSDK_NGX_D3D12_EvaluateFeature(dlssHandle, &dlssParams, nullptr);
```

---

## 6. 🛠️ DRIVER-LEVEL HACKS (+5-20% FPS, 0 усилий)

### 6.1. Linux/RADV (AMD)

```bash
# Принудительный ACO-компилятор (быстрее LLVM)
export RADV_PERFTEST=aco

# Отключение лишних фич драйвера
export RADV_DEBUG=noatocov,nofastclears,nocswave32

# Для игр с большим количеством шейдеров: кэш в RAM
export RADV_PERFTEST=shader_caching_in_memory

# Force GPU clocks (для тестов)
echo "high" | sudo tee /sys/class/drm/card0/device/power_dpm_force_performance_level
echo 7 | sudo tee /sys/class/drm/card0/device/pp_dpm_sclk  # max clock

# Запуск с максимальным FPS
MESA_SHADER_CACHE_DISABLE=1 RADV_PERFTEST=aco ./ps5_relinker
```

### 6.2. Windows/AMD (Adrenalin)

```powershell
# В профиле игры в Adrenalin:
# - Radeon Super Resolution: ON (аналог FSR)
# - Anti-Lag: ON
# - Chill: OFF
# - Image Sharpening: 80%
# - Tessellation: Override -> 8x (PS5-игры часто over-tessellate)

# Реестр: принудительный async compute
# HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Class\{...}\DriverDesc
# Добавить: AsyncComputeScheluding = 1
```

### 6.3. Windows/NVIDIA

```powershell
# В панели NVIDIA:
# - Threaded Optimization: ON
# - Power Management: Prefer Maximum Performance
# - Low Latency Mode: Ultra
# - Texture Filtering: High Performance
# - Anisotropic: Override -> 4x (PS5 часто 16x зря)

# Через nvidia-smi для force clocks
nvidia-smi -lgc 2500,2800  # lock GPU clocks
```

---

## 7. 🌊 WAVE64 → WAVE32 ПРАВИЛЬНЫЙ ФОЛЛБЭК (+10-25% на NVIDIA)

**Суть:** PS5 = wave64. NVIDIA = wave32. Если просто эмулировать 64 через 2×32 — теряем 50% производительности. Правильно — **перекомпилировать шейдер под wave32**.

### 7.1. Статическая перекомпиляция

```cpp
// На этапе загрузки шейдера:
if (caps.vendor == GpuVendor::Nvidia) {
    // Берём pre-compiled wave32 вариант (см. CMake из прошлого ответа)
    shaderModule = LoadSpirv("shader.wave32.spv");
} else {
    // AMD: нативный wave64
    shaderModule = LoadSpirv("shader.wave64.spv");
}
```

### 7.2. HLSL-код с условной компиляцией

```hlsl
#if WAVE_SIZE == 64
    // PS5-оригинал: 64-широкие операции
    float sum = WaveActiveSum(value);
    uint ballot = WaveActiveBallot(condition);
#else
    // NVIDIA-fallback: переписан под 32, но семантически эквивалентно
    float sum = WaveActiveSum(value); // на 32-вейве работает нативно
    uint ballot = WaveActiveBallot(condition);
    
    // Если нужна эмуляция 64-битного ballot:
    // uint2 ballot64 = uint2(WaveActiveBallot(cond), WaveActiveBallot(cond));
    // Но это ДОРОГО — избегайте.
#endif
```

**Прирост:** на NVIDIA — **+15-25%**, потому что убираем эмуляцию.

---

## 8. 💾 MEMORY BANDWIDTH (+10-30% FPS)

**Суть:** PS5 имеет 448 GB/s unified memory. На PC — от 256 (DDR5) до 1000+ GB/s (GDDR6X). Но **латентность** и **кэш-паттерны** важнее.

### 8.1. LDS (Local Data Share) для compute

```hlsl
// ❌ ПЛОХО: каждый поток читает из глобальной памяти
groupshared float sharedData[256];

[numthreads(64, 1, 1)]
void CS_Main(uint tid : SV_GroupThreadID, uint gid : SV_GroupID) {
    // Каждый поток делает 100 чтений из global -> bandwidth killer
    for (int i = 0; i < 100; ++i) {
        float val = globalBuffer[gid * 64 + i]; // 100 global reads!
        sharedData[tid] = val;
        GroupMemoryBarrierWithGroupSync();
        // ...
    }
}

// ✅ ХОРОШО: coalesced read в LDS, потом из LDS
[numthreads(64, 1, 1)]
void CS_Main(uint tid : SV_GroupThreadID, uint gid : SV_GroupID) {
    // 1 coalesced read на workgroup
    sharedData[tid] = globalBuffer[gid * 64 + tid];
    GroupMemoryBarrierWithGroupSync();
    
    // Потом все чтения из LDS (в 100x быстрее)
    for (int i = 0; i < 100; ++i) {
        float val = sharedData[(tid + i) % 64]; // LDS read
        // ...
    }
}
```

**Прирост:** на compute-heavy шейдерах — **+20-30%**.

### 8.2. Texture compression (BC7 → BC1 если возможно)

```cpp
// PS5 часто использует BC7 (высокое качество, 8 bpp)
// На PC можно даунгрейдить до BC1 (1 bpp) для не-критичных текстур
// Прирост: +10-15% bandwidth savings -> +5-10% FPS
```

---

## 9. 📋 FPS-BOOST ЧЕК-ЛИСТ (порядок действий)

```
ШАГ 0: Базовый замер
  □ RenderDoc / Nsight / RDP — снять профиль кадра
  □ Найти самый дорогой pass (обычно: shadows, lighting, post-process)
  □ Замерить occupancy (GPU-Util / aco_print_stats)

ШАГ 1: Driver hacks (0 усилий, +5-20%)
  □ ENV vars (RADV_PERFTEST, NVIDIA settings)
  □ Force GPU clocks
  □ Anti-Lag / Low Latency Mode

ШАГ 2: Occupancy (средние усилия, +30-80%)
  □ Замерить VGPR/SGPR
  □ Применить specialization constants
  □ spirv-opt --scalar-replacement
  □ Цель: occupancy ≥ 50%

ШАГ 3: Async Compute (высокие усилия, +15-40%)
  □ Создать отдельную compute-очередь
  □ Overlap: culling ↔ G-buffer, lighting ↔ particles
  □ Замерить GPU timeline в RenderDoc

ШАГ 4: Early-Z / Mesh Shaders (средние усилия, +20-50%)
  □ Включить VK_EXT_mesh_shader
  □ Primitive culling в mesh shader
  □ Hi-Z occlusion pre-pass

ШАГ 5: Upscaling (низкие усилия, +60-150%)
  □ FSR 3 / DLSS 3
  □ Render at 1080p, output at 4K
  □ Frame Generation

ШАГ 6: Memory (высокие усилия, +10-30%)
  □ groupshared / LDS для compute
  □ Texture compression downgrade
  □ Descriptor buffer вместо descriptor sets
```

---

## 10. 🤖 AI PROMPT (для OpenCode/Cursor — только FPS-буст)

```markdown
# ROLE
Ты — graphics-программист, оптимизирующий FPS в PS5→PC relinker.
Цель: МАКСИМАЛЬНЫЙ FPS на PC (Windows + Linux, AMD + NVIDIA).

# ПРИОРИТЕТЫ (от большего прироста к меньшему)
1. Occupancy / VGPR-оптимизация (+30-80%)
2. Async Compute overlap (+15-40%)
3. Early-Z / Mesh Shaders (+20-50%)
4. FSR 3 / DLSS 3 Frame Gen (+60-150%)
5. Bindless + Descriptor Buffer (+5-15%)
6. Driver-level hacks (+5-20%)
7. Wave64→32 правильный фолбэк (+10-25% на NVIDIA)
8. Memory bandwidth / LDS (+10-30%)

# ЖЁСТКИЕ ПРАВИЛА
1. ЗАПРЕЩЕНО: динамическая ветка wave-size в одном шейдере. Только статические
   вариации (wave64.spv / wave32.spv), выбираемые на этапе создания пайплайна.
2. ЗАПРЕЩЕНО: компиляция пайплайнов в render-тред. Только async + заглушка.
3. ОБЯЗАТЕЛЬНО: specialization constants для всего, что влияет на цикл/ветвление.
   Это даёт скаляризацию -> снижение VGPR -> рост occupancy.
4. ОБЯЗАТЕЛЬНО: NonUniformResourceIndex на каждом divergent-индексе bindless.
5. ОБЯЗАТЕЛЬНО: groupshared (LDS) для compute-шейдеров с повторными чтениями.
6. ОБЯЗАТЕЛЬНО: mesh shader с primitive culling (эмуляция PS5 Geometry Engine).
7. ОБЯЗАТЕЛЬНО: FSR 3 / DLSS 3 integration для upscale + frame gen.
8. МЕТРИКИ: всегда замерять occupancy (aco_print_stats / GPU-Util), FPS,
   GPU timeline (RenderDoc). Без метрик оптимизация вслепую.

# ДРАЙВЕРНЫЕ ХАКИ
Linux/AMD: RADV_PERFTEST=aco, MESA_SHADER_CACHE_DISABLE=1 (для замера),
           echo "high" > power_dpm_force_performance_level
Windows/AMD: Radeon Anti-Lag, Tessellation Override 8x
Windows/NVIDIA: Threaded Optimization ON, Low Latency Ultra, force clocks

# КОД
C++20, Vulkan 1.3, HLSL -> SPIR-V через dxc.
Всегда: RenderDoc-совместимый код (никаких vendor-specific extensions без фолбэка).
```

---

## 🎯 ИТОГО: что делать прямо сейчас

1. **Сними профиль кадра** в RenderDoc / RDP → найди самый дорогой pass.
2. **Замерь occupancy** → если <50%, применяй §1 (VGPR-оптимизация).
3. **Включи FSR 3** → мгновенный +60-150% FPS (§5).
4. **Примени driver hacks** (§6) → +5-20% за 5 минут.
5. **Async compute** (§2) → если GPU простаивает на 30%+.
6. **Mesh shaders + culling** (§3) → если высокий overdraw.

**Одна фраза:** *Occupancy + FSR 3 + Async Compute = +150-300% FPS. Всё остальное — докрутка.*