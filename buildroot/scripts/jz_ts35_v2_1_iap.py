import os

Import("env")

# Keep the JZ-TS35 application in the 0x08008000 IAP layout.
custom_ld_script = os.path.abspath("buildroot/ldscripts/jz_ts35_v2_1_0x8000.ld")
linkflags = list(env.get("LINKFLAGS", []))
script_flag_found = False

for index, flag in enumerate(linkflags):
    flag_text = str(flag)
    if flag_text.startswith("-Wl,-T"):
        linkflags[index] = "-Wl,-T" + custom_ld_script
        script_flag_found = True
    elif flag_text == "-T" and index + 1 < len(linkflags):
        linkflags[index + 1] = custom_ld_script
        script_flag_found = True

if not script_flag_found:
    linkflags.append("-Wl,-T" + custom_ld_script)

env.Replace(LINKFLAGS=linkflags)
