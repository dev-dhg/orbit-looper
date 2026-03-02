import sys
import subprocess
import os

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("Installing Pillow...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "Pillow"])
    from PIL import Image, ImageDraw

def create_icon():
    # 256x256 with transparent background
    img = Image.new('RGBA', (256, 256), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    
    accent_color_alpha = (168, 85, 247, 102) # #a855f7 with 0.4 opacity
    playing_color = (51, 255, 136, 255) # #33ff88
    
    # Background circle with opacity
    # mapped to 256x256: cx=128, cy=128, r=80, stroke=20
    draw.ellipse((48, 48, 208, 208), outline=accent_color_alpha, width=20)
    
    # Green arc overlay
    draw.arc((48, 48, 208, 208), start=270, end=360, fill=playing_color, width=20)
    
    # Rounded end-caps for the arc
    # Top endpoint (270 deg)
    draw.ellipse((118, 38, 138, 58), fill=playing_color)
    # Right endpoint (360/0 deg)
    draw.ellipse((198, 118, 218, 138), fill=playing_color)
    
    # Center dot
    # r=3 -> scaled 8x -> r=24 -> bbox (128-24, 128-24, 128+24, 128+24)
    draw.ellipse((104, 104, 152, 152), fill=playing_color)
    
    os.makedirs('assets', exist_ok=True)
    img.save('assets/icon.png')
    print("Icon generated successfully at assets/icon.png")

if __name__ == "__main__":
    create_icon()
