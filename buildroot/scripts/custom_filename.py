Import("env")

# PlatformIO/SCons may represent CPPDEFINES as strings, tuples or lists.
# Accept all supported forms instead of assuming every define is a pair.
parsed_flags = env.ParseFlags(env.get("BUILD_FLAGS", ""))
defines = {}

for item in (parsed_flags.get("CPPDEFINES") or []):
    if isinstance(item, (tuple, list)):
        if not item:
            continue
        key = str(item[0])
        value = str(item[1]) if len(item) > 1 else ""
    else:
        token = str(item)
        if "=" in token:
            key, value = token.split("=", 1)
        else:
            key, value = token, ""
    defines[key] = value

def clean(value):
    value = str(value).strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
        value = value[1:-1]
    return value

hardware = clean(defines.get("HARDWARE", "firmware"))
version = clean(defines.get("SOFTWARE_VERSION", "dev"))
env.Replace(PROGNAME="{}.{}".format(hardware.replace(" ", "_"), version))
