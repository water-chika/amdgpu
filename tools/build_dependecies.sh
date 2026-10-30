if [[ 'Arch' == $(lsb_release --id --short) ]]; then
	echo 'Arch'
	sudo pacman --sync libdrm clang llvm vulkan-tools mesa vulkan-headers vulkan-devel
elif [[ 'Fedora' == $(lsb_release --id --short) ]]; then
	echo 'Fedora'
	sudo dnf install libdrm-devel clang llvm vulkan-tools mesa-vulkan-drivers vulkan-headers vulkan-loader-devel
else
	echo 'Unknown Linux Distribution'
	echo 'Please install dependencies drm, clang, llvm, vulkan, mesa,'
fi
