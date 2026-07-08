# VideoInfo
Linux utility for extracting metadata from video files (you can also use wsl)



# How to build
```bash
git clone https://github.com/PraVor/vidinf.git
cd vidinf
sudo apt install libavformat-dev libavcodec-dev libavutil-dev
gcc -O2 -o vidinf main.c $(pkg-config --cflags --libs libavformat libavcodec libavutil) -lm
```


# How to use
```bash
./vidinf -<path> [arg]
```



powered by claude.ai 👀
