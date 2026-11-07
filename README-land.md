# ztOS for the Redmi 3S (land)

> [!WARNING]
> This wipes the phone. You'll lose everything that isn't backed up. 

## What works

| Category | Feature | Status | Notes |
|---|---|---|---|
| **Basic** | Flashing | ✅ Works | lk2nd fastboot, `userdata` only |
| | USB networking | ✅ Works | `ssh alarm@172.16.42.1` |
| | Internal storage | ✅ Works | |
| | SD card | ❔ Untested | |
| | Battery | ✅ Works | Level, voltage and charging |
| | Suspend and resume | ✅ Works | Sleep and wake with the power key |
| **Display** | Screen | ✅ Works | 720×1280 EBBG, capacitive keys |
| | Backlight | ✅ Works | Through ztOSIAL |
| | Touchscreen | ✅ Works | FocalTech FT5306 |
| **Multimedia** | 3D acceleration | ✅ Works | freedreno, Adreno 505 |
| | Audio: speaker | ✅ Works | |
| | Audio: earpiece and microphone | ✅ Works | |
| | Audio: headphones | ❔ Untested | |
| | Camera | ❌ Broken | |
| | Camera flash / flashlight | ✅ Works | |
| **Connectivity** | Wi-Fi | ✅ Works | 2.4 GHz only (no 5ghz support on land) |
| | Bluetooth | ⚠️ Partial | Works with headsets |
| | GPS | ✅ Works | Through modem, takes a while |
| | NFC | ➖ N/A | The phone has none |
| **Modem** | SIM | ✅ Works | Logs on to network |
| | Calls | ❌ Broken | No sound |
| | SMS | ❔ Untested | |
| | Mobile data | ✅ Works | |
| **Sensors** | Accelerometer | ✅ Works | |
| | Gyroscope | ✅ Works | |
| | Ambient light | ➖ N/A | No sensor |
| | Proximity | ✅ Works | |
| | Magnetometer | ❌ Broken | Reads 0 |
| | Haptics | ✅ Works | |
| **Miscellaneous** | Notification LED | ✅ Works | all RGB works |
| | USB OTG | ❔ Untested | |
| | ztOSIAL (`ztos-ial test`) | ✅ Works | All interfaces pass |
| | Android apps (Waydroid) | ❌ Broken | Kernel lacks `CONFIG_ANDROID_BINDERFS` |
| | Android apps (ATL) | ❔ Untested | Installed, not yet tried |

✅ Works · ⚠️ Partial · ❌ Broken · ❔ Untested · ➖ Not applicable

## What you need

- A Redmi 3S with an unlocked bootloader (step 1)
- A PC with fastboot
- A cable
- The built images

## Step 0: Building the OS

Clone each of these repos into the same folder, next to each other, and set their respective branches (git switch <branch>).

| Repository | Branch |
|---|---|
| [Noxyntious/ztconflo](https://github.com/Noxyntious/ztconflo/tree/port/xiaomi-daisy) | `port/xiaomi-daisy` |
| [Noxyntious/ztos-devices](https://github.com/Noxyntious/ztos-devices/tree/port/xiaomi-land) | `port/xiaomi-land` |
| [Noxyntious/ztOS](https://github.com/Noxyntious/ztOS/tree/port/xiaomi-daisy) | `port/xiaomi-daisy` |
| [postmarketOS/pmaports](https://gitlab.postmarketos.org/postmarketOS/pmaports) | (the kernel, firmware and lk2nd) |

ztConflo (essentially pmbootstrap but for ztOS) runs on Arch Linux and derivatives (Cachy, Endeavour, Omarchy, Manjaro, Nyarch etc.) ONLY, to use any other distro you need to use distrobox. 

Make sure you are using a rootful distrobox!!! (--root)

ONLY building is done in distrobox, flashing is done outside.

```
distrobox create --root -n ztos-arch-root -i docker.io/library/archlinux:latest -Y
distrobox enter --root ztos-arch-root
sudo pacman -Syu python git
```

Now, in the ztConflo folder:

```
python -m ztconflo build xiaomi-land \
    --registry ../ztos-devices --pmaports ../pmaports --ztos-src ../ztOS --jobs 16
```

If everything goes well, you should hopefully have your images in `out/xiaomi-land/`.

If you run into "No such file or directory" errors, QEMU might have lost its F flag. `grep flags /proc/sys/fs/binfmt_misc/qemu-aarch64`

To reset (outside distrobox):
```
sudo sh -c 'echo -1 > /proc/sys/fs/binfmt_misc/qemu-aarch64'
sudo systemctl restart systemd-binfmt
```


## Step 1: BL unlock
1. In Android, enable Developer Options, go to Mi Unlock, link your account, wait 7 days.
2. Use MiForge/MiUnlockTool on GitHub to unlock (or the official Mi Unlock app).

***This WILL wipe the phone!!! Make sure you have everything saved.***

## Step 2: Flash lk2nd

Make sure you are in the xiaomi-land folder:

```
cd out/xiaomi-land
```

Now, flash lk2nd from fastboot.

```
fastboot flash boot lk2nd.img
```

## Step 3: Flash ztOS

Back in fastboot, flash the rootfs to the `userdata` partition:

```
sh flash.sh
```

Or manually:

```
fastboot -S 256M flash userdata rootfs.img
fastboot reboot
```

The image is pretty big, so give it some time. If one stage gets stuck for more than 5 minutes, use CTRL+C to break out of the flash, reboot your phone back into fastboot, and try again. Perhaps use a different cable or port.

If you see:
```
Warning: skip copying userdata image avb footer due to sparse image.
```
ignore it. It's normal.

## Step 4: First boot

> [!IMPORTANT]
> ztOS does NOT support SIM pins. You will get no popup asking you to unlock your SIM.

To unlock your SIM if it has a code, open the Terminal tile on the Start screen and write this:

```
mmcli -m any                          # "lock: sim-pin" means it wants the PIN
sudo mmcli -i any --pin=1234          # replace with your pin of course.
```

If sudo asks for a password, the password is `alarm`.

To disable SIM pins, add --disable-pin to the end: `sudo mmcli -i any --pin=1234 --disable-pin`

> [!NOTE]
> This will disable your PIN SIM-wide, including on Android.

**Now you are pretty much done!**

### SSH over USB

When the phone is connected to USB, NCM kicks in and gives you network access:

```
ssh alarm@172.16.42.1
```

password `alarm`. You can change it with the `passwd` command.
