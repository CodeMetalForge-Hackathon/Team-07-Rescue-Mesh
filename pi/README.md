# Team 7 base station (Pi 5 or any laptop)

A small Python web server. It talks to the LoRa box plugged in over USB and serves
a phone-friendly web app with four tabs: **Chat**, **Network**, **Map** and **Log**.
It runs with no internet connection, so everything is in `static/index.html`.

## Try it now without any hardware
```bash
python3 base_station.py --sim          # simulated BASE + relay R1 + carried C1
```
Open http://localhost:8000. In the simulation, C1 walks out and back, sends a
HELP after about 25 s, and replies to texts.

## Run it with the real box
```bash
sudo apt install python3-serial        # Pi (usually already installed). Mac/Windows: pip3 install pyserial
python3 base_station.py                # auto-finds the Feather; or: --port /dev/ttyACM0
```
- On a **Pi**, let your user open serial ports once: `sudo usermod -aG dialout $USER`, then log out and back in.
- Close the Arduino Serial Monitor first, because only one program can use the port at a time.
- If the box is unplugged or resets, the server reconnects automatically.
- Every event is saved to `logs/session-*.jsonl`, which is useful for the demo and the write-up.

## Phones: turn the Pi into a Wi-Fi hotspot
Raspberry Pi OS (Bookworm) uses NetworkManager, so creating a hotspot takes one command:
```bash
sudo nmcli device wifi hotspot ifname wlan0 ssid Team7-Base password rescue2026
sudo nmcli connection modify Hotspot connection.autoconnect yes   # come back after reboot
```
1. On the phone, join **Team7-Base** (password `rescue2026`).
2. Open **http://10.42.0.1:8000**.

> While wlan0 is a hotspot, the Pi has **no internet** over Wi-Fi. Install everything
> first, or use Ethernet for internet. To turn the hotspot off: `sudo nmcli connection down Hotspot`.

## Start automatically at boot (optional)
```bash
sudo tee /etc/systemd/system/team7.service >/dev/null <<EOF
[Unit]
Description=Team 7 base station
After=network.target

[Service]
User=$USER
WorkingDirectory=$PWD
ExecStart=/usr/bin/python3 $PWD/base_station.py
Restart=always

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl enable --now team7
journalctl -u team7 -f        # watch its output
```
Run this from inside the `pi/` folder.

## Map calibration
The map turns signal strength into an estimated distance. To make it more accurate:
1. Put two boxes a known distance apart (for example 1 m, or 5 m in a hallway).
2. On the **Map** tab, pick the two boxes, enter the distance, and press **Set**.

Do this once at the venue. The default model is −45 dBm at 1 m, with path-loss exponent 2.7.

## What the box firmware needs to print
See `../PROTOCOL.md`. The current firmware already prints `DATA` lines, so
nodes, temperatures and direct links show up today. The next firmware adds `ME`, `BCN`,
`NBR`, `MSG`, `QM`, `ACK`, `SENT` and `LINK`, and accepts the `TX` / `QM` / `AK` commands.
That turns on texting, quick messages, roles and the full map.
