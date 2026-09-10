# 需求文档

## 引言

本项目旨在以 **ESP-IDF 风格**重写开源项目 WLED 的核心灯控代码,放置于 `wlen_c/` 目录中。代码以 C 语言为主,遵循 ESP-IDF 编码规范与工程结构(esp_err_t 错误处理、组件化目录、IDF CMake 构建系统、ESP_LOGx 日志、FreeRTOS 任务调度),可使用 ESP-IDF 框架自身提供的能力(如 RMT 外设驱动、GPIO、esp_timer),但不引入任何 ESP-IDF 之外的第三方开源组件(不使用 Arduino、FastLED、NeoPixelBus 等)。

**参考源码范围**(`wled00/` 目录):
- 灯带总线管理:`bus_manager.cpp`、`bus_manager.h`、`bus_wrapper.h`
- 效果引擎:`FX.cpp`、`FX.h`、`FX_fcn.cpp`、`FX_2Dfcn.cpp`
- 色彩处理:`colors.cpp`、`colors.h`
- 亮度与过渡:`led.cpp`
- 调色板:`palettes.cpp`

**明确排除范围**:Web 服务器、WebSocket、MQTT、Alexa、E1.31、Hue、IR 遥控、OTA、Presets 文件系统、Usermod 等网络与外围功能均不在本次重写范围内。

**目标平台与交付形态**:交付一个 **ESP-IDF 组件(component)**:`wlen_c/` 本身即为标准 IDF 组件目录,采用 `idf_component_register()` 写法的 `CMakeLists.txt`,对外通过 `include/` 暴露公共 API(统一入口头 `wled_c.h`)。组件可被任意 ESP-IDF 工程以 `EXTRA_COMPONENT_DIRS` 方式引用;演示入口作为组件内可选 example(`examples/` 或 `main/` 示例工程)提供,不作为核心交付。

---

## 需求

### 需求 1 - 工程结构与构建(ESP-IDF 风格)

**用户故事**:作为 ESP-IDF 开发者,需要一个标准 IDF 组件化工程,可直接 `idf.py build` / `idf.py flash monitor` 使用。

#### 验收标准
1. `wlen_c/` 为标准 ESP-IDF 组件目录:含 `CMakeLists.txt`(使用 `idf_component_register()` 声明 SRCFILES、INCLUDE_DIRS、REQUIRES)、`include/`(公共 API 头,统一入口 `wled_c.h`)、`src/`(内部实现)、可选 `Kconfig`(组件配置项)。
2. 全部公共函数返回 `esp_err_t`(成功 ESP_OK,失败带 ESP_ERR_xxx),使用 `ESP_ERROR_CHECK` / `ESP_RETURN_ON_FALSE` 等 IDF 宏。
3. 日志统一使用 `ESP_LOGI/ESP_LOGW/ESP_LOGE` 并带模块 TAG。
4. 面向对象的 C 风格:结构体 + `wled_xxx_init()`/`wled_xxx_deinit()` 句柄式接口,不使用 C++ 特性。

### 需求 2 - WS2812 输出驱动(基于 ESP-IDF RMT)

**用户故事**:作为使用者,需要用 ESP-IDF 自带的 RMT 外设驱动 WS2812 灯带,波形编码符合 WS2812 时序规范。

#### 验收标准
1. 提供总线驱动层(对应 `bus_wrapper.h`/`bus_manager.cpp` 中驱动部分):使用 ESP-IDF RMT(led_strip 风格或 rmt_tx 原始 API)实现 WS2812 像素流发送。
2. 支持配置:GPIO 引脚、像素数、颜色顺序(RGB/GRB/BRG 等)、RGB/RGBW 模式、总线亮度。
3. 发送采用 RMT 编码器将颜色字节转为 WS2812 时序位流,不依赖任何第三方库。
4. 支持至少 4 条总线并存(各自独立 RMT 通道)。

### 需求 3 - 色彩处理模块

**用户故事**:作为灯控框架使用者,需要完整的色彩转换与混合工具,以支撑灯效渲染与调色板展开。

#### 验收标准
1. 提供 `wled_color.h/.c`,实现 32 位 RGBW 颜色打包/解包(对应 WLED 的 `RGBW32`/`R()`/`G()`/`B()`/`W()`)。
2. 实现 HSV→RGB(对应 `colorHSV`,8bit 饱和度/值,支持白色通道推导)、RGBW 提升逻辑(对应 `getRGBW`)、色温近似色(对应 `colorKtoW`)。
3. 实现色彩混合 `wled_color_blend(a, b, ratio)` 与亮度缩放 `wled_color_scale(c, scale)`。
4. 所有函数为纯函数,可独立测试。

### 需求 4 - 调色板模块

**用户故事**:作为灯效开发者,需要一批与 WLED 兼容的经典调色板,供灯效按索引取色。

#### 验收标准
1. 定义调色板数据结构(渐变停色点数组,兼容 WLED 的 16 停色点格式)。
2. 内置不少于 40 个 WLED 经典调色板(如 Rainbow、Rainbow Bands、Sunset、Fire、Lava、Ice、Forest、Party 等),名称与 WLED 保持一致。
3. 实现 `wled_palette_get_color(palette, index)` 按 0-255 索引在停色点间线性插值取色。
4. 支持用户自定义调色板(C 数组编译期注册)。

### 需求 5 - 段(Segment)与灯带控制器(Strip)核心

**用户故事**:作为 WLED 框架研究者,需要重现 WLED 的核心架构:多总线、多段的灯带组织方式,段可独立设置灯效参数。

#### 验收标准
1. 实现段数据结构:起止像素、分组/间隔、偏移、镜像/反转、2D 宽高、激活/选中标志、颜色槽数组(3 个)、当前灯效编号、速度、强度、调色板、过渡时间、运行时状态(帧计数、内部数据指针)。
2. 段支持 2D 映射(宽×高,含转置/翻转标志),提供 `(x,y)→像素索引` 的虚像素寻址。
3. 实现灯带控制器:段集合管理(增删改查)、总线管理器集成、亮度应用(含可选 gamma)、每帧刷新 `wled_strip_service()`。
4. 段的运行时数据由统一分配器管理,段切换灯效时正确重置。
5. 支持最多 32 段,单总线最大 1024 像素(可配置宏提升)。

### 需求 6 - 总线管理模块(BusManager)

**用户故事**:作为多灯带用户,需要总线管理器统一管理多条物理输出。

#### 验收标准
1. 实现总线抽象(总线类型、起始像素、长度、颜色顺序、RGB/RGBW、亮度)。
2. 实现 `show()`:将段渲染结果的全局像素缓冲按颜色顺序重排、做亮度缩放,交给 RMT 驱动发送。
3. 提供 `wled_bus_manager_add()/remove()/get()/show_all()`,支持至少 4 条总线并存。
4. 颜色顺序重排与亮度缩放为纯逻辑,可单元测试。

### 需求 7 - 效果引擎(FX)

**用户故事**:作为用户,需要在段上运行丰富的灯效,参数(速度/强度/调色板/双色)可独立调节,帧率受控。

#### 验收标准
1. 实现效果引擎调度:效果表(编号、名称、函数指针)、`wled_fx_service()` 按帧驱动所有激活段。
2. 实现不少于 35 个代表性灯效,覆盖类别:
   - 静态/闪烁类:static、blink、flash、breath
   - 彩虹/渐变类:rainbow、rainbow cycle、sweep、gradient
   - 扫描/追逐类:scan、dual scan、chase、theater chase、larson scanner
   - 呼吸/脉冲类:running dots、comet、meteor、meteor smooth
   - 火焰/噪声类:fire flicker、firework、noise(近似)
   - 填充类:color wipe、fill up/down、wipe random
   - 混合类:twinkle、sparkle、random dots/blobs
   - 2D 类:基于行列的扫描、简单等离子
3. 每个灯效为 C 函数,使用段运行时数据保持状态。
4. 帧率控制:默认约 42 FPS(对应 WLED 的 `WLED_FPS`),可调节。
5. 支持过渡(fade):段颜色/灯效切换时按过渡时间平滑混合。

### 需求 8 - 亮度、过渡与开关控制

**用户故事**:作为使用者,需要全局亮度调节、开关切换与亮度渐变过渡,行为与 WLED 一致。

#### 验收标准
1. 提供亮度控制模块(对应 `led.cpp` 精简重写):
   - `wled_toggle_on_off()`:亮度为 0 时恢复上次亮度,否则记录并置 0。
   - 全局亮度与过渡:`wled_set_brightness()` 触发按过渡时间的线性渐变,过渡完成应用最终亮度。
   - 亮度乘数(briMultiplier,百分比)缩放。
2. 提供段颜色/参数变更应用接口(对应 `applyValuesToSelectedSegs` 精简版)。

### 需求 9 - 主程序与 FreeRTOS 集成

**用户故事**:作为 ESP32 用户,需要 app_main 启动灯控框架并以独立任务运行刷新循环。

#### 验收标准
1. 组件提供启动接口 `wled_c_start(const wled_c_config_t *cfg)`:内部创建默认配置(用户可指定总线、段、亮度),并在独立 FreeRTOS 任务中运行灯控刷新循环(优先级/栈大小可配置)。
2. 刷新循环以约 42 FPS 调用 `wled_fx_service()` + `wled_strip_service()`,组件内部管理任务生命周期。
3. 组件提供 `wled_c_stop()` 干净地释放资源(删除任务、释放 RMT 通道与内存)。
4. 可选提供 `example/` 示例工程演示:循环切换代表性灯效(每 5 秒),日志输出灯效名称与 FPS 统计。