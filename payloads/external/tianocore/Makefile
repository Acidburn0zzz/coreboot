##
## This file is part of the coreboot project.
##
## Copyright (C) 2016 Google Inc.
##
## This program is free software; you can redistribute it and/or modify
## it under the terms of the GNU General Public License as published by
## the Free Software Foundation; version 2 of the License.
##
## This program is distributed in the hope that it will be useful,
## but WITHOUT ANY WARRANTY; without even the implied warranty of
## MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
## GNU General Public License for more details.
##

# force the shell to bash - the edksetup.sh script doesn't work with dash
export SHELL := env bash

STABLE_COMMIT_ID=315d9d08fd77db1024ccc5307823da8aaed85e2f
TAG-$(CONFIG_TIANOCORE_MASTER)=origin/master
TAG-$(CONFIG_TIANOCORE_STABLE)=$(STABLE_COMMIT_ID)

project_name=Tianocore
project_dir=$(CURDIR)/tianocore
project_git_repo=https://github.com/MattDevo/edk2

export EDK_TOOLS_PATH=$(project_dir)/BaseTools

ifeq ($(CONFIG_TIANOCORE_DEBUG),y)
BUILD_TYPE=DEBUG
else
BUILD_TYPE=RELEASE
endif

all: build

$(project_dir):
	echo "    Cloning $(project_name) from Git"
	git clone $(project_git_repo) $(project_dir)

fetch: $(project_dir)
	cd $(project_dir); \
		git show $(TAG-y) >/dev/null 2>&1 ; \
		if [ $$? -ne 0 ] || [ "$(TAG-y)" = "origin/master" ]; then \
			echo "    Fetching new commits from the $(project_name) repo"; \
			git fetch; \
		fi

checkout: fetch
	echo "    Checking out $(project_name) revision $(TAG-y)"
	cd  $(project_dir); \
		git checkout master; \
		git branch -D coreboot 2>/dev/null; \
		git checkout -b coreboot $(TAG-y)

checktools:
	printf "Checking uuid-dev..."
	echo "#include <uuid/uuid.h>" > libtest.c
	echo "int main(int argc, char **argv) { (void) argc; (void) argv; return 0; }" >> libtest.c
	$(HOSTCC) $(HOSTCCFLAGS) libtest.c -o libtest >/dev/null 2>&1 && printf " found uuid-dev.\n" || \
		( printf " Not found.\n"; echo "ERROR: please_install uuid-dev (uuid-devel)"; exit 1 )
	rm -rf libtest.c libtest
	printf "Checking nasm..."
	type nasm > /dev/null 2>&1 && printf " found nasm.\n" || \
		( printf " Not found.\n"; echo "Error: Please install nasm."; exit 1 )

config: checkout checktools
	unset CC; $(MAKE) -C $(project_dir)/BaseTools
	cd $(project_dir); \
		export EDK_TOOLS_PATH=$(project_dir)/BaseTools; \
		export WORKSPACE=$(project_dir); \
		. ./edksetup.sh BaseTools

build: config
	echo "    build       $(project_name) $(TAG-y)"

ifeq ($(CONFIG_TIANOCORE_GOP_DRIVER), y)
	echo "copying GOP driver from $(CONFIG_TIANOCORE_GOP_FILE)"
	cp ~/coreboot/$(CONFIG_TIANOCORE_GOP_FILE) $(project_dir)/CorebootPayloadPkg/IntelGopDriver.efi
	cp ~/coreboot/$(CONFIG_TIANOCORE_VBT_FILE) $(project_dir)/CorebootPayloadPkg/vbt.bin
else
	echo "not copying GOP driver; config: $(CONFIG_TIANOCORE_GOP_DRIVER)"
endif

ifeq ($(CONFIG_TIANOCORE_TARGET_IA32), y)
	cd $(project_dir); \
		export EDK_TOOLS_PATH=$(project_dir)/BaseTools; \
		export WORKSPACE=$(project_dir); \
		. ./edksetup.sh BaseTools; \
		build -a IA32 -t GCC5 -p CorebootPayloadPkg/CorebootPayloadPkgIa32.dsc -b $(BUILD_TYPE) -n 2
else
	cd $(project_dir); \
		export EDK_TOOLS_PATH=$(project_dir)/BaseTools; \
		export WORKSPACE=$(project_dir); \
		. ./edksetup.sh BaseTools; \
		build -a IA32 -a X64 -t GCC5 -p CorebootPayloadPkg/CorebootPayloadPkgIa32X64.dsc -b $(BUILD_TYPE) -n 2
endif
	mv $(project_dir)/Build/CorebootPayloadPkg*/*/FV/UEFIPAYLOAD.fd $(project_dir)/Build/UEFIPAYLOAD.fd

clean:
	test -d $(project_dir) && (cd $(project_dir); rm -rf Build) || exit 0

distclean:
	rm -rf $(project_dir)

.PHONY: all fetch checkout checktools config build clean distclean
