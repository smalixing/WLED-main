# 实现任务列表 — wled_c ESP-IDF 灯控组件

## 任务概览

- 总任务数:12
- 任务分类:开发任务
- 参考文档:requirements.md(9 项需求)、design.md(9 模块设计)

## 开发任务

- [ ] 1. 组件骨架与公共类型定义
  - 创建 `wlen_c/CMakeLists.txt`(idf_component_register)、`Kconfig`、目录结构(include/src)
  - 编写 `wled_c_types.h`:RGBW32/R/G/B/W 宏、SEG_OPT_* 段选项位、WLEDC_FPS/WLEDC_FRAME_TIME/WLEDC_MAX_SEGMENTS 常量、esp_err_t 风格错误码
  - 验收:CMake 可被 IDF 工程发现(EXTRA_COMPONENT_DIRS),头文件独立可编译
  - _需求:1_

- [ ] 2. 色彩处理纯函数模块
  - 编写 `wled_c_color.c/h`:RGBW32 打包宏、hsv2rgb_spectrum/rainbow(16 位 hue)、rgb2hsv、color_blend/add/fade、fast_color_scale、gamma 查找表、color_kto_rgb、color_balance_from_kelvin
  - 全部自研实现,不移植 FastLED 代码
  - 验收:纯函数无外设依赖,主机端可编译运行
  - _需求:3_

- [ ] 3. 调色板模块
  - 编写 `wled_c_palette.c/h`:16 停色点格式定义、从 WLED palettes.cpp 拷贝 40+ 经典调色板数据(PROGMEM→const)
  - 实现 color_from_palette 线性插值取色、wledc_palette_register 自定义调色板注册(动态 ID 从 200 起)
  - 验收:全部固定调色板可按索引取色,插值结果与停色点定义一致
  - _需求:4_

- [ ] 4. RMT WS2812 驱动
  - 编写 `wled_c_rmt.c`:rmt_tx_channel 初始化(GPIO/12.5MHz/mem_block)、led_strip_encoder(bytes_encoder WS2812 时序 400ns/850ns + copy_encoder + reset 300us)
  - 实现 wledc_rmt_show:wait_all_done 防撕裂 + rmt_transmit
  - 验收:单总线可驱动 WS2812 灯带出正确颜色,帧间无撕裂
  - _需求:2_

- [ ] 5. 总线抽象与总线管理器
  - 编写 `wled_c_bus.c/h`:wledc_bus_config_t 定义(type/pin/start/length/reversed/color_order/brightness)
  - 实现 BusManager:bus_add/bus_remove/bus_get/bus_get_all,最多 4 总线,像素按 start 偏移路由
  - show() 路径批量完成颜色顺序重排(12 种排列)与亮度缩放
  - 验收:多总线并存,跨总线像素正确路由,RMT 细节被完全隐藏
  - _需求:2、6_

- [ ] 6. 段结构与段管理
  - 编写 `wled_c_segment.c/h`:wledc_segment_t(对齐 WLED Segment 字段:colors×3/start/stop/grouping/spacing/options/mode/palette/speed/intensity/custom×3/blend_mode + 运行时 step/call/aux0/aux1/data/pixels)
  - 实现段生命周期:add/remove/get/set_range/set_mode/set_palette/set_options/2D set_dimensions
  - 实现运行时数据管理:效果数据 lazy 分配、效果切换统一重置(对齐 addUsedSegmentData 模式)
  - 验收:32 段创建/修改/删除正常,段字段与需求定义一致
  - _需求:5_

- [ ] 7. 2D 映射与段过渡
  - 扩展 segment:XY(x,y)=x+y*vWidth 索引、transpose/mirror_y/reverse_y 坐标变换
  - 实现段过渡结构:旧段快照 + progress(0-65535)插值,颜色/调色板/亮度混合
  - 受 Kconfig CONFIG_WLEDC_ENABLE_2D 控制
  - 验收:2D 段效果输出正确,效果切换时平滑过渡
  - _需求:5、7(过渡)_

- [ ] 8. 效果引擎框架
  - 编写 `wled_c_fx.c/h`:wledc_fx_fn 函数指针签名、wledc_fx_table 函数表、wledc_fx_run 调度(reset_if_required → 调用效果)
  - 实现 _current_segment 指针 + SEGMENT/SEGENV 风格访问宏(内部)
  - 编写 `wled_c_fx_support.c`:oscillate/beatsin8/beatsin16/sin16 等效果公共工具
  - 验收:框架可注册并运行任意效果函数,返回延迟参与帧率控制
  - _需求:7_

- [ ] 9. 1D 效果实现(35+ 效果)
  - 编写 `wled_c_fx_1d.c`:按 8 类实现静态/呼吸/追逐/闪烁/渐变/扫描类效果,对齐 WLED FX.cpp 对应效果逻辑
  - 每个效果使用调色板取色与 SEGENV 运行时变量
  - 验收:每类效果在真机运行视觉正确,帧率保持 42FPS 基准
  - _需求:7_

- [ ] 10. 2D 效果实现
  - 编写 `wled_c_fx_2d.c`:矩阵/动画类 2D 效果(受 CONFIG_WLEDC_ENABLE_2D 控制)
  - 验收:2D 效果在矩阵布局下输出正确
  - _需求:7_

- [ ] 11. 亮度模块与灯带核心
  - 编写 `wled_c_bri.c`:bri/bri_old/bri_t/bri_last 状态机、toggle_on_off、set_brightness 过渡、bri_multiplier 缩放(对齐 led.cpp,去除夜灯 SUN 模式)
  - 编写 `wled_c_strip.c`:strip_service 主循环(遍历活跃段→fx_run→过渡混合→apply_brightness→bus_show)
  - 验收:亮度渐变平滑,开关记忆正确,全链路数据流贯通
  - _需求:8_

- [ ] 12. 组件 API、刷新任务与示例工程
  - 编写 `wled_c_main.c`:wled_c_start/wled_c_stop、FreeRTOS 任务(vTaskDelayUntil 对齐 23.8ms 帧周期)、互斥锁线程安全
  - 编写 `include/wled_c/wled_c.h` 统一入口(聚合所有子头文件)
  - 创建 `wlen_c/example/`:最小 main.c(配置总线→创建段→设置效果→start)
  - 验收:idf.py build 编译通过,真机 example 运行灯效正常
  - _需求:9、1_

## 依赖关系与执行顺序

- 任务 1 为基础(类型/骨架),必须最先完成
- 任务 2、3、4 相互独立,可并行
- 任务 5 依赖 4;任务 6 依赖 1;任务 7 依赖 6;任务 8 依赖 2、3、6;任务 9、10 依赖 8;任务 11 依赖 5、8;任务 12 依赖 11(集成收尾)