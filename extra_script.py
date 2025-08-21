Import("env")
import os, shutil

# Paths to the build artifacts
build_dir = env.subst("$BUILD_DIR")
bootloader = os.path.join(build_dir, "bootloader.bin")
partitions = os.path.join(build_dir, "partitions.bin")
app = os.path.join(build_dir, "firmware.bin")
merged = os.path.join(build_dir, "HamClockFirmware.bin")

# Destination folder for merged firmware
dest_dir = os.path.join(env.subst("$PROJECT_DIR"), "firmware")
dest_file = os.path.join(dest_dir, "HamClockFirmware.bin")

def merge_bins(source, target, env):
    print(">>> Merging BIN files into single HamClockFirmware.bin...")
    env.Execute(f"esptool.py --chip esp32 merge_bin -o {merged} "
                f"--flash_mode dio --flash_freq 80m --flash_size 4MB "
                f"0x1000 {bootloader} 0x8000 {partitions} 0x10000 {app}")

    # Ensure ./firmware folder exists
    os.makedirs(dest_dir, exist_ok=True)

    # Copy the merged file to ./firmware
    shutil.copyfile(merged, dest_file)
    print(f">>> Copied merged firmware to {dest_file}")

env.AddPostAction("buildprog", merge_bins)
