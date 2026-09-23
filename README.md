# OLED PC Monitor

Монитор состояния ПК для OLED-дисплея, подключённого к **ESP32-C3**.

Эта версия репозитория предназначена прежде всего для **macOS**. Исходная кроссплатформенная версия, которая хорошо работает на Linux/Omarchy, сохранена в ветке [`main`](https://github.com/alex79ven/pc-monitor-esp32/tree/main). macOS-версия находится в ветке [`macos`](https://github.com/alex79ven/pc-monitor-esp32/tree/macos).

## Возможности

- CPU, RAM, использование диска и температура CPU;
- часы при выключенном дисплее;
- анимация звёзд при запуске macOS Screen Saver и блокировке экрана;
- OSD громкости и mute;
- OSD яркости дисплея и подсветки клавиатуры;
- OSD медиа-клавиш play/pause, next и previous;
- название текущего трека и заголовок YouTube;
- отправка данных по USB Serial и BLE;
- автоматическое BLE-переподключение;
- восстановление BLE после системного sleep/wake;
- полностью фоновый режим на macOS без окна приложения;
- запуск при входе в macOS через `LaunchAgent`.

## Архитектура

```text
PC (monitor.py + mac_bridge.py + mac_smc.py)
             │
             ├── USB Serial ──┐
             └── BLE / NUS ───┤
                              ▼
                    ESP32-C3 + FreeRTOS
                              │
                              ▼
                    SSD1306 OLED 72×40
```

Прошивка ESP32 принимает короткие текстовые команды и управляет режимами дисплея. Host-приложение на компьютере собирает метрики, следит за системными событиями и отправляет их по последовательному порту или BLE.

## Оборудование

- ESP32-C3;
- OLED SSD1306 с разрешением 72×40;
- I²C-дисплей с GPIO 5 (`SDA`) и GPIO 6 (`SCL`) по умолчанию;
- USB-кабель для питания и USB Serial;
- ESP32 с поддержкой BLE.

## BLE-протокол

Используется Nordic UART Service (NUS):

| Назначение | UUID |
|---|---|
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| запись host → ESP32 | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |

Примеры команд:

```text
CPU 24 RAM 63 DISK 41 TEMP 60
BRIGHT 44
KBD 52
VOL 31 MUTE 0
MEDIA 2
NOWPLAY 1 Artist - Title
TIME 23:45:00
STARS
```

## Требования macOS

- macOS 14.5 или новее;
- Python 3.9+ для сборки из исходников;
- Xcode Command Line Tools:

```bash
xcode-select --install
```

Собранная и проверенная версия — x86_64. Для Apple Silicon может понадобиться Rosetta 2 или отдельная universal-сборка.

## Сборка DMG

Из корня репозитория:

```bash
bash host/build_macos.sh
```

Скрипт:

1. создаёт `host/.venv-mac`;
2. устанавливает зависимости из `host/requirements-macos.txt`;
3. собирает приложение через PyInstaller;
4. добавляет Bluetooth usage description и информацию о фоновом режиме;
5. подписывает приложение ad-hoc;
6. создаёт `host/dist/OLED-Monitor.dmg`.

## Установка приложения

Откройте DMG и перетащите `OLED-Monitor.app` в `/Applications`.

Для запуска только по BLE:

```bash
/Applications/OLED-Monitor.app/Contents/MacOS/OLED-Monitor --bt --no-serial
```

Доступные параметры:

```text
--port PORT       выбрать USB Serial-порт
--rate RATE       период отправки метрик, секунд
--bt [NAME]       включить BLE, имя по умолчанию OLED-MONITOR
--no-serial       не использовать USB Serial
```

## Автоматический запуск и фоновый режим

В репозитории есть готовый LaunchAgent:

```text
host/com.user.oledmonitor.plist
```

Он запускает приложение после входа пользователя в macOS, не показывает окно и перезапускает процесс при сбое (`KeepAlive`).

Установка вручную:

```bash
cp host/com.user.oledmonitor.plist \
  ~/Library/LaunchAgents/com.user.oledmonitor.plist

launchctl bootstrap "gui/$(id -u)" \
  ~/Library/LaunchAgents/com.user.oledmonitor.plist
```

Приложение должно находиться по пути:

```text
/Applications/OLED-Monitor.app
```

## Сон и пробуждение

После закрытия крышки macOS может приостановить CoreBluetooth-сессию, при этом приложение или ESP32 иногда ещё считают соединение активным. Host-приложение сравнивает время до и после паузы, принудительно сбрасывает зависший BLE-клиент, повторно использует последний адрес устройства и запускает новый поиск с таймаутами.

Прошивка также отслеживает отсутствие BLE-команд. Если соединение не получает данные в течение 5 секунд, ESP32 сбрасывает состояние подключения и снова начинает BLE-рекламу. Поэтому после изменения прошивки её нужно перезаписать на ESP32.

## Разрешения macOS

При первом запуске разрешите приложению:

- **Bluetooth** — System Settings → Privacy & Security → Bluetooth;
- **Accessibility** — System Settings → Privacy & Security → Accessibility;
- при необходимости **Automation** для Safari.

Accessibility требуется для клавиш громкости, яркости и медиа. Bluetooth необходим для отправки данных на ESP32.

## Температура CPU

На Intel Mac температура читается напрямую через SMC/IOKit, без `sudo` и сторонних утилит. Используется ключ `TC0P`, с fallback на `TC0D`, `TC0H`, `TCXC` и `TCXc`.

Температура отображается в строке метрик и на OLED как:

```text
TMP 60
```

SMC и MediaRemote являются приватными API macOS и могут измениться в будущих версиях ОС.

## YouTube и MediaRemote

Для заголовка используется системный MediaRemote API. Если он недоступен, применяется fallback для активной вкладки Safari. В OLED передаётся только текст, без просмотра истории, аккаунта или содержимого видео.

## Режимы OLED

- **Metrics** — CPU, RAM, диск и температура;
- **Clock** — часы при неактивном экране;
- **Stars** — заставка при Screen Saver или блокировке;
- **Media** — исполнитель и название трека/видео;
- **OSD** — временные уведомления о яркости, клавиатуре, громкости и медиа.

## Структура проекта

```text
src/main.cpp                 прошивка ESP32
platformio.ini               настройки PlatformIO
host/monitor.py              сбор метрик и управление BLE/Serial
host/mac_bridge.py           macOS: яркость, клавиши, Screen Saver, MediaRemote
host/mac_smc.py              чтение температуры Intel Mac через SMC
host/build_macos.sh          сборка DMG
host/com.user.oledmonitor.plist  автозапуск
```

## Linux / Omarchy

Исходная версия находится в ветке `main`. Она сохраняет Linux-ориентированный сценарий:

- MPRIS/playerctl для музыки и видео;
- Hyprland/Omarchy idle и lock;
- sysfs для яркости клавиатуры;
- PulseAudio для громкости;
- Linux `/dev/input` для мультимедийных клавиш.

## Ограничения и безопасность

- BLE-сервис не включает pairing и авторизацию; используйте устройство в доверенной близкой среде;
- macOS privacy permissions могут потребовать повторного разрешения после переустановки или пересборки приложения;
- macOS `MediaRemote`, `DisplayServices` и SMC — приватные интерфейсы;
- macOS DMG собирается с ad-hoc подписью и не проходит Developer ID notarization;
- перед публикацией на других компьютерах может потребоваться ручное разрешение Gatekeeper.

## Проверенная конфигурация

- macOS 14.5;
- Intel MacBook Pro 11,2;
- Python 3.9.6;
- PyInstaller 6.22.x;
- ESP32-C3 с OLED SSD1306 72×40;
- BLE-имя `OLED-MONITOR`.

## Лицензия

В репозитории пока не добавлена отдельная лицензия. Перед распространением кода необходимо определить условия лицензирования.
