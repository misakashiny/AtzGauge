# OTA 分块上传 Bug 与补丁（待修，需 ESP-IDF）

> 状态：**已定位、未修复**（本机固件 `main-104-9c3fa1b27e9d`；main 源码 `721c5981` 起同样存在）
> 记录日期：2026-09（实测）
> 关联：`12` 号文档（OTA 接口清单）、`15` 号文档（上车清单）

---

## 一、一句话结论

**`recv timeout limit reached` 分支只 `break`，忘了置 `failed = true`** —— 于是**被截断的块被当成"完整块"接受**，
`s_recv.received_size` 被推进到一个**错误的偏移**，App 之后**永久对不上** `X-Offset`，
拿到 `400 unexpected chunk offset` 后放弃。**三处（firmware / theme / bootmedia）同一个缺陷。**

---

## 二、现象

- App 通过 WiFi OTA 上传固件，**约 35% 处卡住**，最终报：
  `上传失败 http400 error：UNEXPECTED CHUNK OFFSET`
- 每次都在同一位置附近失败（因为 `offset=786432` = 3×256KB = 2,261,424 的 **34.77%**）
- 断开重试仍然是同样结果（App 重发旧 offset，被设备一再拒绝）

---

## 三、日志证据（原始，未删改）

```
W (723838) httpd_txrx: httpd_sock_err: error in recv : 11                      ← errno 11 = EAGAIN
I (723839) ota_wifi: firmware recv #361: to_read=41034 received=-3 remaining=41034 duration=2001069 us
W (723909) ota_wifi: firmware recv timeout 23 (consecutive 1): offset=786432 chunk_received=221110 remaining=41034
I (725114) ota_wifi: firmware recv #362: to_read=41034 received=48 remaining=41034 duration=1051597 us   ← 1.05s 只收 48 字节
...（#361 → #502 共 141 次 recv 调用，只收到约 12KB）
W (754024) httpd_txrx: httpd_sock_err: error in recv : 11
I (754025) ota_wifi: firmware recv #502: to_read=28554 received=-3 remaining=28554 duration=2000601 us
W (754098) ota_wifi: firmware recv timeout 32 (consecutive 3): offset=786432 chunk_received=233590 remaining=28554
E (754251) ota_wifi: firmware recv timeout limit reached: total=32 consecutive=3
I (754348) ota_wifi: firmware chunk complete: offset=786432 len=233590 next=1020022 last=0 (recv took 105901 ms, 502 calls)
W (754496) httpd_txrx: httpd_sock_err: error in send : 104                      ← 回包发送失败 ECONNRESET
I (754564) ota_wifi: firmware: intermediate response sent in 74 ms (handler total 106339 ms)
W (754673) httpd_uri: httpd_uri: uri handler execution failed
E (779515) ota_wifi: firmware chunk out of sequence: offset=786432 expected=1020022 kind=1
W (784034) httpd_txrx: httpd_sock_err: error in recv : 11
E (784039) ota_wifi: firmware chunk out of sequence: offset=786432 expected=1020022 kind=1
```

**关键数字**：块大小 262,144 字节（256KB）。第 4 块 `offset=786432`，
实际只收到 **233,590 / 262,144** 字节（缺 28,554），却被记为 `next=1020022 = 786432 + 233590`。

---

## 四、根因链

| 步 | 设备侧 | App 侧 |
|---|---|---|
| 1 | 收第 4 块。链路塌陷到 **约 2.2 KB/s**（502 次调用 / 105.9 秒），30 次 2 秒超时后中断 | 持续发送中 |
| 2 | `consecutive_timeouts >= 3` → **只 `break`，`failed` 仍为 false** → 把截断块当完整块 | 等响应 |
| 3 | `s_recv.received_size = 786432 + 233590 = 1,020,022` ← **错误偏移** | 等响应 |
| 4 | 回 `{"ok":true,"nextOffset":1020022}`，但发送 **ECONNRESET（104）失败** | **没收到响应** |
| 5 | 认为下一块该从 1,020,022 开始 | 按自己记录的 786,432 **重发** |
| 6 | `received_size(1020022) != chunk_offset(786432)` → **400 unexpected chunk offset** | **报错放弃；重试永远对不上** |

**触发条件**：WiFi 吞吐崩到 ~2KB/s（典型症状：`received=48` 反复出现 + 约 223ms 固定间隔，
是接收窗口塌陷/对端发送被节流的表现）。**慢链路是诱因，缺 `failed = true` 是放大器。**

---

## 五、缺陷位置（三处，同一个 bug）

### 5.1 固件上传 —— `main/app_obd_dsp/ota_wifi_server.c:729`

```c
            if (consecutive_timeouts >= 3) {
                ESP_LOGE(TAG, "firmware recv timeout limit reached: total=%u consecutive=%u",
                         recv_timeouts, consecutive_timeouts);
                break;                                  // ← 缺 failed = true;
            }
```

### 5.2 主题上传 —— 同文件 `:1079`

```c
            if (consecutive_timeouts >= 3) {
                ESP_LOGE(TAG, "theme recv timeout limit reached");
                break;                                  // ← 同一个缺陷
            }
```

### 5.3 开机动画上传 —— 同文件 `:1418`

```c
            if (consecutive_timeouts >= 3) {
                ESP_LOGE(TAG, "bootmedia recv timeout limit reached: total=%u consecutive=%u",
                         recv_timeouts, consecutive_timeouts);
                break;                                  // ← 同一个缺陷
            }
```

---

## 六、补丁

### 6.1 必须改（三处，各加一行）

```diff
             if (consecutive_timeouts >= 3) {
                 ESP_LOGE(TAG, "firmware recv timeout limit reached: total=%u consecutive=%u",
                          recv_timeouts, consecutive_timeouts);
+                // 截断的块必须判为失败：否则下面的 !failed 分支会把它当成
+                // "完整块"，把 received_size 推进到错误偏移，App 之后永久对不上
+                // X-Offset，只能收到 400。三处（firmware/theme/bootmedia）都要加。
+                failed = true;
                 break;
             }
```

改完后，截断块会走 `if (failed)` 分支 → `recv_reset()` → App 收到明确的 `receive failed`，
可以**从 offset 0 干净重来**，而不是陷进死循环。

### 6.2 建议改（让 App 能自动续传，向后兼容）

在 `unexpected chunk offset` 的 400 响应上**加一个响应头**（不改动错误字符串，避免打断现有 App 的匹配）：

```c
} else if (s_recv.kind != RECV_KIND_FIRMWARE || !s_recv.upload_buf ||
           s_recv.expected_size != expected_size || s_recv.received_size != chunk_offset ||
           memcmp(s_recv.expected_sha, expected_sha, sizeof(expected_sha)) != 0) {
    ESP_LOGE(TAG, "firmware chunk out of sequence: offset=%lu expected=%lu kind=%d",
             (unsigned long)chunk_offset, (unsigned long)s_recv.received_size, s_recv.kind);

    // 告诉 App 设备真正期望的偏移，让它能续传而不是死磕旧 offset
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "%lu", (unsigned long)s_recv.received_size);
    httpd_resp_set_hdr(req, "X-Expected-Offset", hdr);

    return send_err(req, "unexpected chunk offset");
}
```

> ⚠️ 前端要用 JS 读到这个头，还需要在 `set_cors_headers()` 里把它加进
> `Access-Control-Expose-Headers`（该函数已在文件里，第 444 行附近有同类写法）。

### 6.3 可选改进

| 项 | 说明 |
|---|---|
| 超时阈值 | 现在是 `recv_wait_timeout = 2s` × 连续 3 次 = 6 秒。慢链路（本例 2KB/s）必然误判。可改成"按已耗时/剩余字节动态判断"，或把连续次数放大到 5~10 |
| 速率日志 | 每块结束时打印平均速率（`chunk_received / 耗时`），便于一眼区分"链路慢"和"对端停了" |
| 进度可见性 | `GET /ota/status` 需要 token 才能查，出错时用户看不到 `received/expected`，排查困难 |

---

## 七、不改代码的临时规避

| 措施 | 说明 |
|---|---|
| **退出 OTA 页再重进** | 退出会 `esp_restart()`，清掉被污染的 `s_recv` 状态；新一轮从 `offset=0` 走 `recv_reset()` 重新初始化 |
| **手机开飞行模式 → 只开 WiFi** | 杜绝 Android 切回蜂窝导致连接断 |
| **关掉手机蓝牙** | BLE 与 WiFi 共用 2.4GHz 射频 |
| **关省电模式 + 屏幕常亮 + App 留前台** | 防止系统限制网络 |
| **手机贴近仪表（10~30cm）** | SoftAP 发射功率小 |
| **换一部手机** | 不同机型 WiFi 栈差异极大，这条往往最有效 |
| **干脆不用 WiFi OTA** | 直接用 USB：`python -m esptool --chip esp32s3 -p COM3 write-flash 0x20000 <固件.bin>` |

---

## 八、可直接贴到 GitHub 的 issue 报告

**标题**

```
WiFi OTA: truncated chunk is accepted on recv timeout, corrupting the upload offset (400 unexpected chunk offset)
```

**正文**

```markdown
### Summary
A WiFi OTA firmware upload from the companion app reliably fails around 35% with
`400 unexpected chunk offset`. Root cause is that the `recv timeout limit reached`
branch only `break`s out of the receive loop **without setting `failed = true`**,
so a truncated chunk is treated as a complete one and `s_recv.received_size` is
advanced by the partial byte count. The app then resends its own (correct) offset,
which will never match again.

### Version
- Device: `main-104-9c3fa1b27e9d` (release `firmware/release/obd_brz_gauge.bin`, sha256 `e129c594…`)
- The same code is present on `main` as of `721c5981` (2026-09-01), i.e. not yet fixed.
- Affects all three upload paths: firmware, theme, bootmedia.

### Expected
A chunk interrupted by consecutive recv timeouts should be reported as failed, so
the client can restart cleanly from offset 0.

### Actual
The truncated chunk is logged as `firmware chunk complete` and `s_recv.received_size`
is advanced by the partial length. The client's next request (its own correct offset)
is rejected with `unexpected chunk offset`, and every retry fails the same way.

### Log (device side)
```
E (754251) ota_wifi: firmware recv timeout limit reached: total=32 consecutive=3
I (754348) ota_wifi: firmware chunk complete: offset=786432 len=233590 next=1020022 last=0 (recv took 105901 ms, 502 calls)
W (754496) httpd_txrx: httpd_sock_err: error in send : 104
E (779515) ota_wifi: firmware chunk out of sequence: offset=786432 expected=1020022 kind=1
E (784039) ota_wifi: firmware chunk out of sequence: offset=786432 expected=1020022 kind=1
```
Note `len=233590` while the chunk was 262144 bytes (`remaining=28554` at the break).

### Root cause
`main/app_obd_dsp/ota_wifi_server.c` — three identical sites
(`:729` firmware, `:1079` theme, `:1418` bootmedia):

```c
if (consecutive_timeouts >= 3) {
    ESP_LOGE(TAG, "firmware recv timeout limit reached: ...");
    break;                    // <-- missing: failed = true;
}
```

Because `failed` stays false, the following block runs:

```c
if (!failed) { ESP_LOGI(TAG, "firmware chunk complete: ..."); }
...
if (!is_last && s_recv.received_size < s_recv.expected_size) {
    snprintf(json, ..., "{\"ok\":true,\"nextOffset\":%lu}", s_recv.received_size);
```

### Suggested fix
1. Set `failed = true;` before the three `break`s.
2. Optionally return the expected offset so the client can resume instead of
   resending a stale offset, e.g. add an `X-Expected-Offset` response header on the
   400 path (and expose it via `Access-Control-Expose-Headers` in `set_cors_headers()`).
3. Consider relaxing the timeout policy: `recv_wait_timeout` is 2 s and 3 consecutive
   timeouts = 6 s total, which aborts mid-chunk on a slow (≈2 KB/s) link.

### Extra context
In this occurrence the link degraded to ~2.2 KB/s (48-byte reads, ~223 ms apart,
`httpd_sock_err: error in recv : 11` = EAGAIN), so a slow link is what triggered it —
but the missing `failed = true` is what turns a recoverable hiccup into a permanent
offset mismatch.
```

---

## 九、修完怎么验证

1. 编译烧录后，故意制造慢链路（手机远离仪表 / 开多个 WiFi 占用）
2. 期望日志：`recv timeout limit reached` 之后紧接 **`firmware recv failed`**，
   而**不再**出现 `chunk complete ... next=<错误值>`
3. 期望 App 能收到明确失败并从 offset 0 重来，最终**要么传完、要么报"传输中断"**，
   而不是卡在 35% 后报 offset 错
