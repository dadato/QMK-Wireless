/* 2.4G 配对 / 绑定 —— 协议与掉电记忆实现（见 bridge_rf24pair.h 头部说明） */
#include "bridge_rf24pair.h"
#include "bridge_types.h"
#include "bridge_port.h"
#include "bridge_crc16.h"
#include "bridge_config.h"
#include "bridge_log.h"
#include <string.h>

#define BRIDGE_MOD_PAIR "pair"

#define RF24_BOND_MAGIC   0x5232u   /* "R2" */
#define RF24_BOND_VERSION 1u
#define RF24_ID_LEN       4u

static bridge_rf24_bond_t s_bond;
static bool               s_pending_rand_valid;
static uint8_t            s_pending_rand[RF24_ID_LEN];
static uint32_t           s_rand_ctr;

/* 芯片标识：MAC 末 4B（每片唯一；主机桩返回固定 MAC 不影响协议逻辑） */
static void chip_id4(uint8_t out[RF24_ID_LEN])
{
    uint8_t mac[6];

    if (port_chip_get_mac(mac) == BRIDGE_OK) {
        memcpy(out, &mac[2], RF24_ID_LEN);
    } else {
        memset(out, 0, RF24_ID_LEN);
    }
}

static int bond_store(void)
{
    s_bond.crc = bridge_crc16_ccitt((const uint8_t *)&s_bond,
                                    (uint16_t)(sizeof(s_bond) - sizeof(s_bond.crc)));
    return port_nv_write(BRIDGE_NV_ID_RF24_BOND, &s_bond, (uint16_t)sizeof(s_bond));
}

void bridge_rf24pair_init(void)
{
    bridge_rf24_bond_t tmp;

    memset(&s_bond, 0, sizeof(s_bond));
    s_bond.magic   = RF24_BOND_MAGIC;
    s_bond.version = RF24_BOND_VERSION;
    s_bond.bound   = 0u;

    memset(&tmp, 0, sizeof(tmp));
    if (port_nv_read(BRIDGE_NV_ID_RF24_BOND, &tmp, (uint16_t)sizeof(tmp)) == BRIDGE_OK) {
        uint16_t crc = bridge_crc16_ccitt((const uint8_t *)&tmp,
                                          (uint16_t)(sizeof(tmp) - sizeof(tmp.crc)));
        if (tmp.magic == RF24_BOND_MAGIC && tmp.version == RF24_BOND_VERSION &&
            crc == tmp.crc) {
            s_bond = tmp;
        }
    }
    LOG_I(BRIDGE_MOD_PAIR, "init bound=%u", s_bond.bound);
}

bool bridge_rf24pair_is_bound(void)
{
    return (s_bond.bound != 0u);
}

uint16_t bridge_rf24pair_link_id(void)
{
    if (!s_bond.bound) {
        return 0u;
    }
    return bridge_crc16_ccitt(s_bond.key, RF24_ID_LEN);
}

const bridge_rf24_bond_t *bridge_rf24pair_get(void)
{
    return &s_bond;
}

void bridge_rf24pair_begin_pairing(void)
{
    /* 每个配对窗口生成新随机数：避免复用旧 rand 造成误配 */
    uint32_t r = (uint32_t)(port_millis() ^ (s_rand_ctr++ * 2654435761u));

    s_pending_rand_valid = true;
    memcpy(s_pending_rand, &r, RF24_ID_LEN);
}

void bridge_rf24pair_build_req(uint8_t *out, uint8_t *out_len)
{
    uint8_t id[RF24_ID_LEN];

    if (out == 0 || out_len == 0) {
        return;
    }
    if (!s_pending_rand_valid) {
        bridge_rf24pair_begin_pairing();
    }
    chip_id4(id);
    out[0] = RF24_PAIR_REQ;
    memcpy(&out[1], id, RF24_ID_LEN);
    memcpy(&out[1u + RF24_ID_LEN], s_pending_rand, RF24_ID_LEN);
    *out_len = (uint8_t)(1u + 2u * RF24_ID_LEN);
}

int bridge_rf24pair_on_frame(const uint8_t *p, uint8_t len,
                             uint8_t *resp, uint8_t *out_len)
{
    if (p == 0 || resp == 0 || out_len == 0 || len == 0u) {
        return 0;
    }
    *out_len = 0u;

    if (p[0] == RF24_PAIR_REQ) {
#if BRIDGE_BOARD_IS_DONGLE
        uint8_t id[RF24_ID_LEN];
        uint8_t key[RF24_ID_LEN];
        uint8_t i;
        /* Dongle：收到键盘请求 → 派生密钥并落盘，回应答帧 */
        if (len < 1u + 2u * RF24_ID_LEN) {
            return 0;
        }
        chip_id4(id);
        for (i = 0u; i < RF24_ID_LEN; i++) {
            key[i] = (uint8_t)(p[1u + i] ^ p[1u + RF24_ID_LEN + i] ^ id[i]);
        }
        bridge_rf24pair_store(key, &p[1], &p[1u + RF24_ID_LEN]);

        resp[0] = RF24_PAIR_RESP;
        memcpy(&resp[1], &p[1], RF24_ID_LEN);                         /* 回显 kbd_id */
        memcpy(&resp[1u + RF24_ID_LEN], &p[1u + RF24_ID_LEN], RF24_ID_LEN); /* 回显 rand */
        memcpy(&resp[1u + 2u * RF24_ID_LEN], key, RF24_ID_LEN);
        *out_len = (uint8_t)(1u + 3u * RF24_ID_LEN);
        return 1;
#else
        /* 键盘收到其它键盘的 REQ：忽略（多键盘并存） */
        return 0;
#endif
    }

    if (p[0] == RF24_PAIR_RESP) {
#if !BRIDGE_BOARD_IS_DONGLE
        /* 键盘：校验 rand 与本次配对一致后落盘 */
        if (len < 1u + 3u * RF24_ID_LEN) {
            return 0;
        }
        if (!s_pending_rand_valid ||
            memcmp(&p[1u + RF24_ID_LEN], s_pending_rand, RF24_ID_LEN) != 0) {
            return 0;   /* rand 不匹配（陈旧应答 / 串扰） */
        }
        bridge_rf24pair_store(&p[1u + 2u * RF24_ID_LEN], &p[1], s_pending_rand);
        s_pending_rand_valid = false;
        return 1;
#else
        return 0;
#endif
    }

    return 0;
}

void bridge_rf24pair_store(const uint8_t key[RF24_ID_LEN],
                           const uint8_t peer[RF24_ID_LEN],
                           const uint8_t rand[RF24_ID_LEN])
{
    s_bond.magic   = RF24_BOND_MAGIC;
    s_bond.version = RF24_BOND_VERSION;
    s_bond.bound   = 1u;
    memcpy(s_bond.key, key, RF24_ID_LEN);
    memcpy(s_bond.peer_id, peer, RF24_ID_LEN);
    memcpy(s_bond.rand, rand, RF24_ID_LEN);
    (void)bond_store();
    LOG_I(BRIDGE_MOD_PAIR, "bound peer=%02X%02X%02X%02X",
          peer[0], peer[1], peer[2], peer[3]);
}

void bridge_rf24pair_forget(void)
{
    if (!s_bond.bound) {
        return;
    }
    memset(&s_bond, 0, sizeof(s_bond));
    s_bond.magic   = RF24_BOND_MAGIC;
    s_bond.version = RF24_BOND_VERSION;
    s_bond.bound   = 0u;
    (void)bond_store();
    LOG_I(BRIDGE_MOD_PAIR, "unbound");
}
