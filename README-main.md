# OLED PC Monitor — Linux / Omarchy

Оригинальная Linux-версия проекта для отправки показателей компьютера на внешний OLED-дисплей через ESP32-C3.

Эта документация описывает ветку [`main`](https://github.com/alex79ven/pc-monitor-esp32/tree/main) — исходную версию для Linux. Проект проверялся на **Omarchy Linux 4** с Hyprland. Ветка [`macos`](https://github.com/alex79ven/pc-monitor-esp32/tree/macos) содержит отдельную macOS-версию приложения и не должна смешиваться с этой инструкцией.

## Возможности

- загрузка CPU, RAM, использования диска и температуры;
- отображение часов, когда данные временно не поступают;
- анимация звёзд при запуске Screen Saver или блокировки экрана;
- OSD яркости дисплея и подсветки клавиатуры;
- OSD громкости и mute;
- обработка мультимедийных клавиш play/pause, next и previous;
- название текущего трека и YouTube-видео через MPRIS;
- отправка данных по USB Serial или BLE;
- автоматический поиск и повторное подключение к BLE-устройству;
- поддержка Hyprland и Omarchy;
- фоновая работа без графического интерфейса.

## Архитектура

```text
Linux / Omarchy 4
  monitor.py
      │
      ├── USB Serial ───────────┐
      └── BLE / Nordic UART ────┤
                                ▼
                         ESP32-C3
                                │
                                ▼
                    OLED SSD1306 72×40
```

ESP32 получает короткие текстовые команды и управляет режимами дисплея. Linux-приложение собирает метрики, следит за состоянием Hyprland/Omarchy и системными настройками, а затем отправляет команды на OLED.

## Совместимость

Проверенная среда:

- **Omarchy Linux 4**;
- Hyprland;
- MacBook Pro Retina 15″ (`MacBookPro11,2`);
- ESP32-C3;
- SSD1306 OLED с разрешением 72×40.

Подробное описание процессора, памяти и графики намеренно не приводится — Linux-часть проекта от них не зависит.

## Оборудование

- ESP32-C3 с поддержкой BLE;
- OLED SSD1306 72×40;
- USB-кабель для питания и USB Serial;
- I²C-соединение между ESP32-C3 и OLED.

Для схемы из `platformio.ini` используются:

- `GPIO5` — SDA;
- `GPIO6` — SCL;
- скорость USB Serial — `115200`.

## BLE-протокол

Используется Nordic UART Service (NUS):

| Назначение | UUID |
|---|---|
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| запись host → ESP32 | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| уведомления ESP32 → host | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |

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

## Требования Linux

### Системные компоненты

Для полной интеграции нужны:

- Python 3;
- `psutil`;
- `pyserial`;
- `bleak`;
- `playerctl`;
- `busctl`;
- `pactl` или PulseAudio;
- `hyprctl`;
- `omarchy-shell`;
- Bluetooth-сервис BlueZ;
- PlatformIO для прошивки ESP32.

Температура CPU читается через модули Linux и `psutil.sensors_temperatures()`. На некоторых ноутбуках может понадобиться пакет `lm-sensors`.

### Проверка инструментов Omarchy

```bash
command -v hyprctl
command -v omarchy-shell
command -v playerctl
command -v pactl
command -v bluetoothctl
```

## Сборка и прошивка ESP32

Установите PlatformIO и подключите ESP32-C3 по USB.

Из корня репозитория:

```bash
pio run
pio run -t upload
```

Если порт не определяется автоматически:

```bash
pio run -t upload --upload-port /dev/ttyACM0
```

Также можно указать порт вручную в `platformio.ini` или при запуске монитора.

## Установка Linux-приложения

Создайте виртуальное окружение:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install psutil pyserial bleak
```

Запустите приложение в BLE-режиме:

```bash
python host/monitor.py --bt
```

По умолчанию ищется устройство с именем:

```text
OLED-MONITOR
```

Другое имя:

```bash
python host/monitor.py --bt "My-OLED"
```

Только BLE, без USB Serial:

```bash
python host/monitor.py --bt OLED-MONITOR --no-serial
```

## Параметры запуска

```text
--port PORT       выбрать USB Serial-порт
--rate RATE       период отправки метрик, секунд
--bt [NAME]       включить BLE, по умолчанию OLED-MONITOR
--no-serial       отключить USB Serial
```

Если USB Serial не найден, приложение продолжает работать через BLE. При подключении USB-порта приложение пытается открыть его автоматически.

## Интеграция с Omarchy и Hyprland

### Screen Saver и блокировка

Linux-версия определяет состояние через:

- `hyprctl clients -j` и класс `org.omarchy.screensaver`;
- `pgrep -f org.omarchy.screensaver`;
- `omarchy-shell lock status`;
- `hyprctl monitors -j` для состояния DPMS.

При Screen Saver или блокировке OLED получает команду `STARS`. После разблокировки приложение возвращает обычный режим.

### Яркость и клавиатура

Используются sysfs-пути:

```text
/sys/class/backlight/*
/sys/class/leds/*kbd_backlight
```

Изменения яркости дисплея и подсветки клавиатуры отправляются на OLED как `BRIGHT` и `KBD`.

### Громкость и mute

Состояние звука читается через PulseAudio:

```bash
pactl get-sink-volume @DEFAULT_SINK@
pactl get-sink-mute @DEFAULT_SINK@
```

### Музыка и видео

Метаданные читаются через `playerctl` и MPRIS. При воспроизведении OLED получает:

```text
NOWPLAY 1 Artist - Title
```

### Мультимедийные клавиши

Linux-версия читает `/dev/input/event*`. Если доступ к устройствам ввода запрещён, мультимедийные клавиши не будут работать, но BLE, метрики, часы и Screen Saver продолжат работать.

Проверка доступа:

```bash
ls -l /dev/input/event*
```

Не добавляйте пользователя в группу `input` без необходимости: это даёт широкий доступ к клавиатуре и другим input-устройствам.

## Автозапуск в Omarchy

Для фоновой работы можно использовать пользовательский systemd-сервис. Создайте:

```text
~/.config/systemd/user/oled-monitor.service
```

Пример:

```ini
[Unit]
Description=OLED PC Monitor
After=graphical-session.target

[Service]
Type=simple
WorkingDirectory=%h/pc-monitor-esp32
ExecStart=%h/pc-monitor-esp32/.venv/bin/python %h/pc-monitor-esp32/host/monitor.py --bt OLED-MONITOR
Restart=always
RestartSec=5

[Install]
WantedBy=default.target
```

Запуск:

```bash
systemctl --user daemon-reload
systemctl --user enable --now oled-monitor.service
```

Путь к репозиторию в `WorkingDirectory` и `ExecStart` нужно заменить на свой.

## Режимы OLED

- **Metrics** — CPU, RAM, диск и температура;
- **Clock** — часы после длительного отсутствия данных;
- **Stars** — заставка при Screen Saver или блокировке;
- **Media** — исполнитель и название трека/видео;
- **OSD** — временные события яркости, клавиатуры, звука и медиа.

## Структура проекта

```text
src/main.cpp       прошивка ESP32
platformio.ini     настройки PlatformIO
host/monitor.py    Linux/Windows/macOS host-приложение
host/build_macos.sh  macOS-сборка
host/build_windows.bat Windows-сборка
host/mac_bridge.py macOS-специфичный код
host/win_bridge.py Windows-специфичный код
```

## Решение проблем

### ESP32 не найден по BLE

```bash
bluetoothctl
bluetoothctl devices
```

Проверьте, что:

- Bluetooth включён;
- имя ESP32 совпадает с `OLED-MONITOR` или переданным `--bt NAME`;
- устройство не подключено к другому BLE-клиенту;
- `bleak` установлен в активном виртуальном окружении.

### Не работает USB Serial

Обычно порт находится в `/dev/ttyACM*` или `/dev/ttyUSB*`:

```bash
ls -l /dev/ttyACM* /dev/ttyUSB*
```

Передайте путь явно:

```bash
python host/monitor.py --port /dev/ttyACM0
```

### Не отображается температура

Проверьте:

```bash
sensors
python3 -c 'import psutil; print(psutil.sensors_temperatures())'
```

Если датчики не видны, установите `lm-sensors` или проверьте загрузку соответствующих модулей ядра.

### Не работают MPRIS или media keys

Проверьте `playerctl`, `busctl` и права доступа к `/dev/input/event*`.

### Не определяется Screen Saver

Проверьте наличие:

```bash
hyprctl clients -j
pgrep -af org.omarchy.screensaver
omarchy-shell lock status
```

## Ограничения

- BLE-сервис не использует pairing и авторизацию;
- USB-пути и sysfs-пути различаются между дистрибутивами;
- MPRIS требует поддержки со стороны медиаплеера;
- мультимедийные клавиши требуют доступа к `/dev/input`;
- температура зависит от доступных Linux-датчиков;
- на версии `main` нет macOS-автозапуска и SMC-интеграции — они находятся в ветке `macos`.

## Лицензия

В репозитории пока не добавлена отдельная лицензия. Перед распространением необходимо определить условия лицензирования.
