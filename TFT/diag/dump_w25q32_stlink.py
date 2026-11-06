#!/usr/bin/env python3
"""Read-only full 4 MiB W25Q32 dump via STM32F207 SRAM mailbox and ST-LINK."""
import argparse, hashlib, os, shutil, subprocess, sys, tempfile, time
from pathlib import Path

MAILBOX, BUFFER = 0x2001F000, 0x20010000
CAPACITY, CHUNK = 0x00400000, 0x00008000
MAGIC, MARKER = 0x57323551, 0xA55A5AA5

def find_cli(explicit):
    candidates = [explicit, os.environ.get("STM32_CUBE_PROGRAMMER_CLI"),
        r"C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe",
        r"C:\ST\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"]
    for p in candidates:
        if p and Path(p).is_file(): return str(Path(p))
    p = shutil.which("STM32_Programmer_CLI") or shutil.which("STM32_Programmer_CLI.exe")
    if p: return p
    raise FileNotFoundError("找不到 STM32_Programmer_CLI.exe；请使用 --cli 指定完整路径")

def run(cli, *args):
    cmd = [cli, "-c", "port=SWD", "freq=50", "mode=HOTPLUG", *map(str, args)]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, encoding="utf-8", errors="replace")
    if p.returncode: raise RuntimeError("CubeProgrammer 执行失败：\n" + p.stdout[-3000:])
    return p.stdout

def read_mem(cli, addr, size, path, width=8):
    run(cli, f"-r{width}", hex(addr), str(size), str(path))
    data = Path(path).read_bytes()
    if len(data) != size: raise RuntimeError(f"读取长度错误：{len(data)} / {size}")
    return data

def write32(cli, addr, value):
    run(cli, "-w32", hex(addr), hex(value))

def main():
    ap = argparse.ArgumentParser(description="W25Q32 全片只读导出（ST-LINK）")
    ap.add_argument("-o", "--output", default="W25Q32_full_dump.bin")
    ap.add_argument("--cli", help="STM32_Programmer_CLI.exe 完整路径")
    ap.add_argument("--timeout", type=int, default=30, help="每块等待秒数")
    args = ap.parse_args()
    cli, output = find_cli(args.cli), Path(args.output).resolve()
    print("ST-LINK:", cli)
    print("目标：4,194,304 bytes；32 KiB/块；共 128 块")
    print("请确认 STM32 已运行 W25Q32 dump 固件，保持 MCU 运行，不要 Halt。")

    with tempfile.TemporaryDirectory(prefix="w25q32_swd_") as td:
        td = Path(td); tmp = td / "mem.bin"
        mb = read_mem(cli, MAILBOX, 32, tmp, 32)
        w = [int.from_bytes(mb[i:i+4], "little") for i in range(0, 32, 4)]
        if w[0] != MAGIC or w[7] != MARKER:
            raise RuntimeError(f"SRAM mailbox 不匹配：{[hex(x) for x in w]}")
        if w[4] != 0 or w[6] != 0x00EF4016:
            raise RuntimeError(f"JEDEC 检测未通过：status={w[4]}, ID=0x{w[6]:08X}")
        print("JEDEC ID EF 40 16：通过，开始读取")

        with output.open("wb") as out:
            for offset in range(0, CAPACITY, CHUNK):
                length = min(CHUNK, CAPACITY - offset)
                write32(cli, MAILBOX + 4, 0)
                write32(cli, MAILBOX + 8, offset)
                write32(cli, MAILBOX + 12, length)
                write32(cli, MAILBOX + 4, 1)
                deadline, status = time.time() + args.timeout, 0
                while time.time() < deadline:
                    status = int.from_bytes(read_mem(cli, MAILBOX + 16, 4, tmp, 32)[:4], "little")
                    if status in (2, 3): break
                    time.sleep(0.3)
                if status != 2: raise RuntimeError(f"块 {offset//CHUNK+1} 失败/超时，status={status}")
                meta = read_mem(cli, MAILBOX + 20, 8, tmp, 32)
                got_offset = int.from_bytes(meta[:4], "little")
                got_length = int.from_bytes(meta[4:8], "little")
                if (got_offset, got_length) != (offset, length):
                    raise RuntimeError(f"块信息不匹配：offset=0x{got_offset:X}, length={got_length}")
                data = read_mem(cli, BUFFER, length, tmp, 8)
                out.write(data); out.flush()
                write32(cli, MAILBOX + 4, 0)
                done = offset + length
                print(f"{done:>9,}/{CAPACITY:,} bytes ({done*100/CAPACITY:5.1f}%)")
    if output.stat().st_size != CAPACITY:
        raise RuntimeError(f"文件大小错误：{output.stat().st_size}")
    digest = hashlib.sha256(output.read_bytes()).hexdigest()
    print("\nDUMP 完成：", output)
    print("大小：4,194,304 bytes")
    print("SHA-256：", digest)

if __name__ == "__main__":
    try: main()
    except Exception as e:
        print("\nERROR:", e, file=sys.stderr); sys.exit(1)
