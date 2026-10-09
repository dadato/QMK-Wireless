/* 2.4G 配对 / 绑定 —— 协议与掉电记忆（core 层，仅依赖 bridge_port.h）
 *
 * 目标（清单 §6.4 / 用户需求）：
 *   1. 键盘与接收器**显式配对**，未绑定设备互不互通（防同环境多套串扰）
 *   2. 绑定密钥 + 对端标识**掉电保存**（DataFlash），重启后凭密钥直接建链
 *
 * 帧格式（rf24 数据面扩展，type 之后固定 2 字节 link_id 小端）：
 *   [type][link_id(2B)][payload...]
 *     - 数据帧 link_id = CRC16(绑定密钥)；与本地不符的帧由 RF 层丢弃
 *     - 配对帧 link_id = 0xFFFF（任何状态下可收发）
 *
 * 配对流程（键盘发起 / Dongle 应答，与 SK32 的 CMD_PAIR_START / Dongle 配对键一致）：
 *   KBD (2G4_PAIR) 周期发 PAIR_REQ: [0x01][kbd_id 4B][rand 4B]
 *   DGL (2G4_PAIR) 收到 REQ → 派生密钥 → 落盘 → 回 PAIR_RESP
 *                  [0x02][kbd_id 4B][rand 4B][key 4B]
 *   KBD 收到 RESP 且 rand 匹配 → 落盘 → 双方进入已绑定数据面
 *
 * 密钥派生（Dongle 侧；仅用于链路区分，非安全用途）：
 *   key[i] = kbd_id[i] ^ rand[i] ^ dgl_id[i]
 *   kbd_id / dgl_id = 各自芯片 MAC 末 4B（每片唯一）
 */
#ifndef BRIDGE_RF24PAIR_H
#define BRIDGE_RF24PAIR_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------- 配对帧子类型（payload 首字节） ---------------- */
#define RF24_PAIR_REQ    0x01u   /* 键盘 → Dongle：[sub][kbd_id 4][rand 4]（9B） */
#define RF24_PAIR_RESP   0x02u   /* Dongle → 键盘：[sub][kbd_id 4][rand 4][key 4]（13B） */

/* ---------------- 绑定记录（NV 槽 BRIDGE_NV_ID_RF24_BOND，64B 槽足够） ---------------- */
typedef struct {
    uint16_t magic;          /* RF24_BOND_MAGIC */
    uint8_t  version;
    uint8_t  bound;          /* 1 = 已绑定 */
    uint8_t  key[4];         /* 链路密钥（CRC16 → link_id） */
    uint8_t  peer_id[4];     /* 对端芯片 ID（MAC 末 4B） */
    uint8_t  rand[4];        /* 本次配对随机数 */
    uint16_t crc;            /* CRC16(前 16B) */
} bridge_rf24_bond_t;

/* ---------------- 接口 ---------------- */
void     bridge_rf24pair_init(void);       /* 上电从 NV 装载（无效即未绑定） */
bool     bridge_rf24pair_is_bound(void);
uint16_t bridge_rf24pair_link_id(void);    /* CRC16(key,4)；未绑定 = 0 */
const bridge_rf24_bond_t *bridge_rf24pair_get(void);

/* 进入配对窗口时调用：生成新 rand，防复用旧随机数造成误配 */
void     bridge_rf24pair_begin_pairing(void);
/* 键盘侧构造 PAIR_REQ（写入 out，长度出 out_len） */
void     bridge_rf24pair_build_req(uint8_t *out, uint8_t *out_len);
/* 处理收到的配对帧（callbacks 层调用）：
 *   返回 1 = 本次配对完成（本端已落盘密钥）；0 = 忽略/未完成。
 *   应答帧写入 resp/out_len（仅 Dongle 收到 REQ 时产出）。 */
int      bridge_rf24pair_on_frame(const uint8_t *p, uint8_t len,
                                  uint8_t *resp, uint8_t *out_len);
/* 直接落盘（配对完成时由 on_frame 内部调用；供测试/调试） */
void     bridge_rf24pair_store(const uint8_t key[4], const uint8_t peer[4],
                               const uint8_t rand[4]);
/* 解绑（CMD_UNPAIR(2G4)）：清密钥并落盘 */
void     bridge_rf24pair_forget(void);

#endif /* BRIDGE_RF24PAIR_H */
