## Pörssisähkönäyttö ESP32 + 1" oled

credit [@TuomasKyl](https://github.com/TuomasKyl) for V1 MVP, non-github version, of this code, which this repo aims to build upon.

Shows the current Finnish spot electricity price (c/kWh, incl. VAT + your extra cost) and today's hourly chart on a 128×64 SSD1306 OLED (SDA 21, SCL 22). Prices come from [spot-hinta.fi](https://spot-hinta.fi).

### Build and flash

```
pio run -t upload && pio device monitor
```

### First setup

1. On first boot (or if WiFi fails) the device opens the hotspot **SpotPrice-Setup**.
2. Join it from a phone. The setup page should open by itself; if not, browse to `http://192.168.4.1`.
3. Pick your network, enter the password, and press Connect. The device restarts and joins it.

### Settings page

Open `http://spotprice.local`, or the IP shown on the screen at boot. Settings are stored in flash:

- **Extra cost (c/kWh):** added to the spot price, e.g. transfer, electricity tax and seller margin.
- **Screen brightness:** low, medium or high.
- **Night mode:** the screen turns off between the chosen times. The device and web page stay online.
- **Change WiFi network:** forgets WiFi and restarts into the setup hotspot.

### BOOT button

- Short press: info screen (IP, data status). At night it shows the prices for 30 s instead.
- Hold for 5 s: forget WiFi and restart into setup.
