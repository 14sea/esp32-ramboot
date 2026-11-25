#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 一次「和弦」：同時按下的修飾鍵 + 至多一個普通鍵（key=0 表示只按修飾鍵） */
typedef struct {
    uint8_t mod;
    uint8_t key;
} kvm_chord_t;

/*
 * 解析 "ctrl+alt+delete"、"F12"、"shift+tab"、"a"、"=" 這類寫法，大小寫不敏感。
 * 單個可打印字符按美式鍵盤佈局換算，需要 Shift 的字符（如 ":"）自動帶上 Shift。
 */
bool kvm_parse_chord(const char *s, kvm_chord_t *out);

/* ASCII 字符 → 和弦（美式佈局）。不支持的字符返回 false */
bool kvm_ascii_chord(char c, kvm_chord_t *out);
