/* CH582F（键盘侧 / Dongle）OTA 分区与 DataFlash 标志位布局 ——「一个常驻 BOOT」
 *
 * CodeFlash 448KB 划分（键盘侧与接收器 Dongle 共用同一套）：
 *   0x00000000 + 8K     BOOT：常驻引导（键盘 iap_main.c / Dongle iap_usb_main.c）
 *                       复位后每次经过：查标志 → 必要时把 Stage 搬到 APP_A → 跳 App
 *   0x00002000 + 216K   APP_A = 当前 App（keyboard/dongle 目标链接地址）
 *   0x00038000 + 216K   Stage = 暂存区（收到的整包镜像写到这里，待 BOOT 搬运）
 *
 * 升级流程（本工程实现；键盘=UART、Dongle=USB HID，两阶段标志驱动）：
 *   ① App 收「进入 OTA」请求（键盘 CMD_ENTER_BOOT / Dongle HID ENTER_OTA）
 *      → 写标志 UART_IAP(键盘) 或 USB_IAP(Dongle) → 软复位；
 *   ② BOOT 见该标志即驻留**收包模式**（键盘 iap_uart_ota_run / Dongle iap_usb_main），
 *      收 FW_BEGIN/FW_DATA/FW_END → 写入 Stage → CRC 校验通过 → 写标志 IAP → 复位；
 *   ③ BOOT 见标志=IAP：复验 Stage CRC → Stage 按记录长度搬 APP_A（1KB 分块）
 *      → 标志=A → 擦 Stage → 跳 0x2000 新 App。
 *   断电安全：先写 IAP 再搬运 → 下次开机自动重试；Stage 在标志置 A 之后才擦；
 *   取消 / 失败 / 空闲超时（60s）写 A 回原 App（APP_A 自始未动，非「单区无回滚」）。
 *   BOOT 重跑期间不发周期心跳，仅应答 CMD_PING（回 EVT_PONG）；不支持的 CMD 忽略不应答。
 *
 * DataFlash 标志位（绝对地址 0x77200，本工程 NV 预留槽 8..11 的整页页首；
 * 整页擦除不伤配置槽 1..4 / 用户区 25..47 / BLE SNV 槽 48..63，且与边界对齐）：
 *   [0]     flag（1=A / 2=B / 3=IAP / 4=USB_IAP / 5=UART_IAP）
 *   [1..4]  镜像长度 u32 LE（Stage 中有效字节数）
 *   [5..6]  CRC-16/CCITT（对长度内数据）
 *   [7]     保留 0
 */
#ifndef BRIDGE_OTA_LAYOUT_H
#define BRIDGE_OTA_LAYOUT_H

/* ---------------- CodeFlash 分区 ---------------- */
#define BRIDGE_OTA_BOOT_ADDR     0x00000000u
#define BRIDGE_OTA_BOOT_SIZE     0x00002000u   /* 8K  */

#define BRIDGE_OTA_APP_ADDR      0x00002000u
#define BRIDGE_OTA_APP_MAX       0x00036000u   /* 216K */

#define BRIDGE_OTA_STAGE_ADDR    0x00038000u
#define BRIDGE_OTA_STAGE_MAX     0x00036000u   /* 216K */

/* ---------------- DataFlash 标志记录 ----------------
 * 偏移相对 DataFlash 基址（0x70000）；**必须落在 256B 页首**：
 * 0x7200 = 绝对 0x77200，落在本工程 NV **预留区**（槽 5..24）中的整页（槽 8..11）。
 * 与三处均不重叠：配置槽 1..4、用户存储区 25..47、BLE SNV 槽 48..63
 * （BLE SNV = 相对 0x7C00..0x7FFF，SDK BLE_SNV_ADDR/BLOCK/NUM = 4 块）。
 * 历史教训（真机）：① 曾用 0x7FC0（页内偏移 0xC0），EEPROM_ERASE(0x7FC0,256)
 * 不满足“页对齐”语义，实测在 OTA 写标志后键盘侧 SDI 两线调试失联（疑越界触及
 * InfoFlash 芯片配置区）；② 曾用页对齐的 0x7F00，但它落在第 4 块 BLE SNV 内，
 * 整页擦除会抹掉 BLE 绑定 —— 现改到预留区页对齐地址，并加编译期断言防回归。 */
#define BRIDGE_OTA_FLAG_REL      0x00007200u
#define BRIDGE_OTA_FLAG_PAGE     0x100u        /* DataFlash 擦除粒度 256B */
#define BRIDGE_OTA_REC_LEN       8u

#if ((BRIDGE_OTA_FLAG_REL & 0xFFu) != 0u)
#error "OTA 标志位必须 256B 页对齐（见上方注释）"
#endif
#if ((BRIDGE_OTA_FLAG_REL + BRIDGE_OTA_FLAG_PAGE) > 0x8000u)
#error "OTA 标志页不得越出 DataFlash（32KB）末尾"
#endif
/* 必须整体落在 NV 预留区（槽 5..24 = 相对 0x7140..0x763F），不得侵入
 * 配置槽 1..4、用户区 25..47 或 BLE SNV（相对 0x7C00 起）。 */
#if (BRIDGE_OTA_FLAG_REL < 0x7140u) || ((BRIDGE_OTA_FLAG_REL + BRIDGE_OTA_FLAG_PAGE) > 0x7C00u)
#error "OTA 标志页必须整体落在 NV 预留槽 5..24，不得侵入配置/用户区或 BLE SNV"
#endif

#define BRIDGE_OTA_FLAG_A        0x01u
#define BRIDGE_OTA_FLAG_B        0x02u
#define BRIDGE_OTA_FLAG_IAP      0x03u   /* 暂存就绪：BOOT 只做 Stage→APP_A 搬运（键盘 UART OTA） */
#define BRIDGE_OTA_FLAG_USB_IAP  0x04u   /* 请求进 USB HID 接收模式（Dongle 侧 App→BOOT） */
#define BRIDGE_OTA_FLAG_UART_IAP 0x05u   /* 请求进 UART 收包模式（键盘 App→BOOT，镜像 USB_IAP） */

#endif /* BRIDGE_OTA_LAYOUT_H */
