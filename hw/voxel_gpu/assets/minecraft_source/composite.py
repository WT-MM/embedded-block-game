from PIL import Image

for container_name, full_name, out_name in [
    ("container.png", "full.png", "heart.png"),
    ("food_empty.png", "food_full.png", "drumstick.png")
]:
    container = Image.open(container_name).convert("RGBA")
    full = Image.open(full_name).convert("RGBA")
    
    # Composite full over container
    comp = Image.alpha_composite(container, full)
    
    padded = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    padded.paste(comp, (0, 0))
    padded.save(out_name)

