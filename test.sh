./make.sh
adb push gammapad /data/local/tmp/
adb shell su -c "chmod +x /data/local/tmp/gammapad"
#adb shell su -c "/data/local/tmp/gammapad retrogame_joypad --ffdev=sc27xx:vibrator"
