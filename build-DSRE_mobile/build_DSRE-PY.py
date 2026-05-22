import subprocess, sys
subprocess.run("{} -m buildozer android init".format(sys.executable), shell=True)
input("please write buildozer.spec, press enter to start build android apk")
subprocess.run("{} -m buildozer android debug".format(sys.executable), shell=True)