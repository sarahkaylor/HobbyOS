import string

# Minimal 8x8 font approximation. We can use a library to render or just use standard base64/hex of a public domain 8x8 font.
# I will download a simple 8x8 font from a public domain source if possible, or just generate one using PIL.
try:
    from PIL import Image, ImageDraw, ImageFont
    img = Image.new('1', (8 * 96, 8), color=0)
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("Courier", 8)
    except:
        font = ImageFont.load_default()
    
    for i in range(32, 128):
        d.text(((i-32)*8, 0), chr(i), font=font, fill=1)
        
    print("const uint8_t font8x8[96][8] = {")
    for i in range(32, 128):
        print("    {", end="")
        for y in range(8):
            val = 0
            for x in range(8):
                if img.getpixel(((i-32)*8 + x, y)):
                    val |= (1 << x)
            print(f"0x{val:02X}", end=", ")
        print("}, // " + (chr(i) if chr(i) != '\\' else '\\\\'))
    print("};")
except Exception as e:
    print(e)
