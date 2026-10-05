# stb_image_write, vendored

The screenshot hotkey's PNG encoder (`src/shot.c`, plorpos-gkd.86.2). Neither
device image ships a libpng diatom could count on, and one header compiled in
costs less than shipping and locating a shared object on every port.

- Upstream: https://github.com/nothings/stb
- Version: `stb_image_write.h` v1.16, at commit
  `2c980bb59875b0d32144a71867fbdebb2f77cd20`, unmodified.
- **Public domain (Unlicense) or MIT**, the reader's choice - the license text
  is at the end of the header. Taken as MIT here, like the rest of diatom's
  permissive code; brings no copyleft in.

`stb_image_write.c` is this repo's, not upstream's: the single translation unit
that defines `STB_IMAGE_WRITE_IMPLEMENTATION`, compiled on its own flags (the
header is not written to this project's warnings) and with stdio compiled out -
`shot.c` writes through `stbi_write_png_to_func` so it can sync before the
rename.
