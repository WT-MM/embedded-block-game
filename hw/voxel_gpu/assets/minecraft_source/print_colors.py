from PIL import Image
for name in ["heart.png", "drumstick.png"]:
    print(f"Colors in {name}:")
    img = Image.open(name)
    colors = set()
    for pixel in img.getdata():
        if pixel[3] > 128:
            colors.add(pixel[:3])
    for c in sorted(colors):
        print(c)
