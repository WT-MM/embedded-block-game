from PIL import Image

for name, out in [("full.png", "heart.png"), ("food_full.png", "drumstick.png")]:
    img = Image.open(name).convert("RGBA")
    print(f"Colors in {name}:")
    colors = set()
    for pixel in img.getdata():
        if pixel[3] > 128:
            colors.add(pixel[:3])
    for c in sorted(colors):
        print(c)
    
    padded = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    padded.paste(img, (0, 0))
    padded.save(out)
