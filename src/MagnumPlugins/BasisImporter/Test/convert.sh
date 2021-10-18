#!/bin/bash

set -e

# RGB
basisu rgb-63x27.png -output_file rgb.basis -y_flip
basisu rgb-63x27.png -output_file rgb.ktx2  -y_flip -ktx2
# basisu doesn't write KTXorientation metadata in KTX2 but that's the only way
# for the plugin to detect y-flip on import. Patch it in manually for testing.
sed -b 's/\x1f\x00\x00\x00KTXwriter\x00Basis Universal /\x12\x00\x00\x00KTXorientation\x00ru\x00\x00\x00\x07\x00\x00\x00_\x00/' rgb.ktx2 > rgb.ktx2.tmp
mv rgb.ktx2.tmp rgb.ktx2
# Without y-flip
basisu rgb-63x27.png -output_file rgb-noflip.basis
basisu rgb-63x27.png -output_file rgb-noflip.ktx2 -ktx2
# Linear image data
basisu rgb-63x27.png -output_file rgb-linear.basis -y_flip -linear
basisu rgb-63x27.png -output_file rgb-linear.ktx2  -y_flip -linear -ktx2

# RGBA
basisu rgba-63x27.png -output_file rgba.basis -force_alpha -y_flip
basisu rgba-63x27.png -output_file rgba.ktx2  -force_alpha -y_flip -ktx2

# UASTC
basisu rgba-63x27.png -output_file rgba-uastc.basis -uastc -force_alpha -y_flip
basisu rgba-63x27.png -output_file rgba-uastc.ktx2  -uastc -force_alpha -y_flip -ktx2

# Multiple images, not possible with KTX2
basisu rgba-63x27.png rgba-27x63.png -output_file rgba-2images-mips.basis -y_flip -mipmap -mip_smallest 16 -mip_filter box

# 2D array
basisu rgba-63x27.png rgba-63x27-slice1.png rgba-63x27-slice2.png -tex_type 2darray -output_file rgba-array.basis -force_alpha -y_flip
basisu rgba-63x27.png rgba-63x27-slice1.png rgba-63x27-slice2.png -tex_type 2darray -output_file rgba-array.ktx2  -force_alpha -y_flip -ktx2

# 2D array with mipmaps
basisu rgba-63x27.png rgba-63x27-slice1.png rgba-63x27-slice2.png -tex_type 2darray -output_file rgba-array-mips.basis -force_alpha -y_flip -mipmap -mip_smallest 16 -mip_filter box
basisu rgba-63x27.png rgba-63x27-slice1.png rgba-63x27-slice2.png -tex_type 2darray -output_file rgba-array-mips.ktx2  -force_alpha -y_flip -mipmap -mip_smallest 16 -mip_filter box -ktx2

# Video
basisu rgba-63x27.png rgba-63x27-slice1.png rgba-63x27-slice2.png -tex_type video -output_file rgba-video.basis -force_alpha -y_flip
basisu rgba-63x27.png rgba-63x27-slice1.png rgba-63x27-slice2.png -tex_type video -output_file rgba-video.ktx2  -force_alpha -y_flip -ktx2

# 3D
basisu rgba-63x27.png rgba-63x27-slice1.png rgba-63x27-slice2.png -tex_type 3d -output_file rgba-3d.basis -force_alpha -y_flip
basisu rgba-63x27.png rgba-63x27-slice1.png rgba-63x27-slice2.png -tex_type 3d -output_file rgba-3d.ktx2  -force_alpha -y_flip -ktx2

# 3D with mipmaps
basisu rgba-63x27.png rgba-63x27-slice1.png rgba-63x27-slice2.png -tex_type 3d -output_file rgba-3d-mips.basis -force_alpha -y_flip -mipmap -mip_smallest 16 -mip_filter box
basisu rgba-63x27.png rgba-63x27-slice1.png rgba-63x27-slice2.png -tex_type 3d -output_file rgba-3d-mips.ktx2  -force_alpha -y_flip -mipmap -mip_smallest 16 -mip_filter box -ktx2

# Cube map
basisu rgba-27x27.png rgba-27x27-slice1.png rgba-27x27-slice2.png rgba-27x27.png rgba-27x27-slice1.png rgba-27x27-slice2.png -tex_type cubemap -output_file rgba-cubemap.basis -force_alpha -y_flip
basisu rgba-27x27.png rgba-27x27-slice1.png rgba-27x27-slice2.png rgba-27x27.png rgba-27x27-slice1.png rgba-27x27-slice2.png -tex_type cubemap -output_file rgba-cubemap.ktx2  -force_alpha -y_flip -ktx2

# Cube map array
# Second layer has the 2nd and 3rd face switched
basisu rgba-27x27.png rgba-27x27-slice1.png rgba-27x27-slice2.png rgba-27x27.png rgba-27x27-slice1.png rgba-27x27-slice2.png rgba-27x27.png rgba-27x27-slice2.png rgba-27x27-slice1.png rgba-27x27.png rgba-27x27-slice1.png rgba-27x27-slice2.png -tex_type cubemap -output_file rgba-cubemap-array.basis -force_alpha -y_flip
basisu rgba-27x27.png rgba-27x27-slice1.png rgba-27x27-slice2.png rgba-27x27.png rgba-27x27-slice1.png rgba-27x27-slice2.png rgba-27x27.png rgba-27x27-slice2.png rgba-27x27-slice1.png rgba-27x27.png rgba-27x27-slice1.png rgba-27x27-slice2.png -tex_type cubemap -output_file rgba-cubemap-array.ktx2  -force_alpha -y_flip -ktx2

# Required for PVRTC1 target, which requires pow2 dimensions
basisu rgb-64x32.png -output_file rgb-pow2.basis -y_flip
basisu rgb-64x32.png -output_file rgb-pow2.ktx2  -y_flip -ktx2
basisu rgb-64x32.png -output_file rgb-linear-pow2.basis -y_flip -linear
basisu rgb-64x32.png -output_file rgb-linear-pow2.ktx2  -y_flip -linear -ktx2
basisu rgba-64x32.png -output_file rgba-pow2.basis -force_alpha -y_flip
basisu rgba-64x32.png -output_file rgba-pow2.ktx2  -force_alpha -y_flip -ktx2