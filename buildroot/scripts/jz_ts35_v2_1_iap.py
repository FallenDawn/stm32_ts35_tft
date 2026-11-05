import os

Import("env")

# Direct boot: vectors at 0x08000000, app code at 0x08008000, settings sector preserved at 0x08004000.
custom_ld_script = os.path.abspath("buildroot/ldscripts/jz_ts35_v2_1_0x8000.ld")
linkflags = list(env.get("LINKFLAGS", []))
script_flag_found = False
index = 0

while index < len(linkflags):
    flag_text = str(linkflags[index])
    if flag_text in ("-T", "-Wl,-T"):
        if index + 1 < len(linkflags):
            linkflags[index + 1] = custom_ld_script
            script_flag_found = True
            index += 2
            continue
    elif flag_text.startswith("-Wl,-T,"):
        linkflags[index] = "-Wl,-T," + custom_ld_script
        script_flag_found = True
    elif flag_text.startswith("-Wl,-T"):
        linkflags[index] = "-Wl,-T," + custom_ld_script
        script_flag_found = True
    index += 1

if not script_flag_found:
    linkflags.append("-Wl,-T," + custom_ld_script)

env.Replace(LINKFLAGS=linkflags)
