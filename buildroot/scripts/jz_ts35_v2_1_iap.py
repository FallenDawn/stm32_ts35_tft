import os

Import("env")

# JZ-TS35 V2.1 application image: retain the existing 0x08008000
# application offset until the board's boot chain is positively identified.
custom_ld_script = os.path.abspath("buildroot/ldscripts/jz_ts35_v2_1_0x8000.ld")

for i, flag in enumerate(env["LINKFLAGS"]):
    if "-Wl,-T" in flag:
        env["LINKFLAGS"][i] = "-Wl,-T" + custom_ld_script
    elif flag == "-T":
        env["LINKFLAGS"][i + 1] = custom_ld_script
