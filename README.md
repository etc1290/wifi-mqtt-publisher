# WiFi MQTT Publisher

基於 ESP-IDF 的 ESP32-C3 範例專案：讀取 DHT11 溫濕度感測器，並透過 WiFi 以 MQTT 定期發佈 JSON 資料。

## 功能

- WiFi Station 模式連線，斷線自動重試（超過重試上限後重新啟動）
- MQTT 客戶端，支援選用的帳號／密碼驗證與自動重連
- 每 N 秒讀取 DHT11 並發佈 JSON
- 所有參數皆可透過 `menuconfig` 設定

## 硬體需求

| 項目 | 說明 |
| --- | --- |
| 開發板 | ESP32-C3（`IDF_TARGET = esp32c3`） |
| 感測器 | DHT11 |
| 接線 | DHT11 DATA → GPIO4（預設，可於 menuconfig 修改）、VCC → 3.3V、GND → GND |

## 專案結構

```
.
├── CMakeLists.txt
└── main
    ├── main.c            # WiFi / MQTT 初始化與發佈任務
    ├── dht11.c / dht11.h # DHT11 驅動
    ├── Kconfig.projbuild # menuconfig 設定項目
    └── CMakeLists.txt
```

## 快速開始

需先安裝並載入 [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/)（`. $IDF_PATH/export.sh`）。

```bash
idf.py set-target esp32c3
idf.py menuconfig          # 進入 "WiFi-MQTT Publisher Configuration" 設定
idf.py build
idf.py -p <PORT> flash monitor
```

專案也提供 `.devcontainer`，可於 VS Code 搭配 ESP-IDF 擴充套件使用。

## 切換 Branch 測試不同版本韌體

不同版本的韌體放在不同的 branch，可切換 branch 後重新編譯、燒錄來測試：

```bash
git branch -a                  # 列出所有 branch
git checkout <branch-name>     # 切換到想測試的版本
idf.py build
idf.py -p <PORT> flash monitor
```

| Branch | 說明 |
| --- | --- |
| `main` | 基礎版：WiFi + MQTT + DHT11，參數透過 menuconfig 於編譯時設定 |
| `WebUI` | 新增 NVS 設定儲存與網頁設定入口：WiFi 或 MQTT 連線失敗（重試 `WIFI_MAX_RETRY` 次，預設 5）時，裝置進入 AP 模式並開啟設定網頁，可於瀏覽器填寫 WiFi / MQTT 參數，儲存後自動重啟；menuconfig 內的值僅作為預設值 |
| `low-power-mode` | 新增 Deep Sleep 低功耗模式：每次讀取並發佈後進入深度睡眠，`SENSOR_READ_INTERVAL_SEC` 改為睡眠時間；另新增狀態指示 LED（`LED_GPIO` 預設 8、`LED_ACTIVE_LOW` 可設定低電位點亮） |

> 切換前請先 commit 或 stash 本地修改。若不同版本的 Kconfig 選項有差異，建議先執行 `idf.py fullclean`（或刪除 `sdkconfig`）再重新 `menuconfig`，避免沿用舊設定。

## 設定項目

位於 menuconfig → **WiFi-MQTT Publisher Configuration**：

| 選項 | 預設值 | 說明 |
| --- | --- | --- |
| `WIFI_SSID` | `myssid` | WiFi 名稱 |
| `WIFI_PASSWORD` | `mypassword` | WiFi 密碼 |
| `WIFI_MAX_RETRY` | `10` | 最大重連次數 |
| `MQTT_BROKER_URI` | `mqtt://test.mosquitto.org:1883` | MQTT Broker 位址 |
| `MQTT_TOPIC` | `home/esp32/dht11` | 發佈主題 |
| `MQTT_USERNAME` | 空 | 選用，Broker 帳號 |
| `MQTT_PASSWORD` | 空 | 選用，Broker 密碼 |
| `DHT11_GPIO` | `4` | DHT11 資料腳位 |
| `SENSOR_READ_INTERVAL_SEC` | `10` | 讀取／發佈間隔（秒） |

> 預設 Broker 為公開的 `test.mosquitto.org`，任何人皆可訂閱，請勿用於敏感資料，並建議自訂專屬 Topic。

## MQTT 負載格式

```json
{"temperature":25.0,"humidity":60.0,"count":1}
```

`count` 為成功讀取的累計次數。可用以下指令驗證：

```bash
mosquitto_sub -h test.mosquitto.org -t "home/esp32/dht11"
```

## 運作流程

1. 初始化 NVS 與 WiFi，等待取得 IP
2. 啟動 MQTT 客戶端
3. 每隔 `SENSOR_READ_INTERVAL_SEC` 秒讀取 DHT11，成功則發佈；讀取失敗或 MQTT 未連線時略過該次並於下個週期重試
