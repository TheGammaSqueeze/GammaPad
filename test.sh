./make.sh
adb push gammapad /data/tmp/
adb shell su -c "chmod +x /data/tmp/gammapad"
#adb shell su -c "/data/tmp/gammapad retrogame_joypad --ffdev=sc27xx:vibrator"
