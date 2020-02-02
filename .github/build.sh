#!bin/bash

RUN_DIR=$PWD
export IDF_PATH=${RUN_DIR}/esp-idf
TOOLCHAIN_DIR=${RUN_DIR}/toolchain
BUILD_DIR=${RUN_DIR}/build

# export IDF_PATH=~/esp-idf
# TOOLCHAIN_DIR=~/esp/crosstool-NG/builds/xtensa-esp32-elf
# BUILD_DIR=${RUN_DIR}/build-test

if [ -d ${BUILD_DIR} ]; then
    echo "Cleaning up ${BUILD_DIR}"
    rm -rf ${BUILD_DIR}
fi

mkdir -p ${BUILD_DIR} ${BUILD_DIR}/config

# install GCC 8.2.0 toolchain
if [ ! -f ${TOOLCHAIN_DIR}/bin/xtensa-esp32-elf-gcc ]; then
    echo "Toolchain not found, downloading..."
    curl -k https://dl.espressif.com/dl/xtensa-esp32-elf-gcc8_2_0-esp-2019r2-linux-amd64.tar.gz \
        -o ${RUN_DIR}/xtensa-esp32-elf-gcc8_2_0-esp-2019r2-linux-amd64.tar.gz
    mkdir -p ${TOOLCHAIN_DIR}
    echo "Installing toolchain..."
    tar -C ${TOOLCHAIN_DIR} zxf ${RUN_DIR}/xtensa-esp32-elf-gcc8_2_0-esp-2019r2-linux-amd64.tar.gz
    echo "Adding ${TOOLCHAIN_DIR}/xtensa-esp32-elf/bin to the path"
    # add toolchain to the path
    export PATH=${TOOLCHAIN_DIR}/xtensa-esp32-elf/bin:${PATH}
fi


# clone ESP-IDF
if [ ! -d ${IDF_PATH} ]; then
    echo "ESP-IDF not available, cloning..."
    git clone -b release/v4.0 --recurse-submodules https://github.com/espressif/esp-idf.git -j4 ${IDF_PATH}
    cd ${IDF_PATH} && python -m pip install -r requirements.txt
fi

# generate config.env file for confgen.py and cmake
echo "Generating config.env"
cat > ${BUILD_DIR}/config.env <<CONFIG_ENV_EOF
{
    "COMPONENT_KCONFIGS": "$(find ${IDF_PATH}/components -name Kconfig -printf '%p ')",
    "COMPONENT_KCONFIGS_PROJBUILD": "${RUN_DIR}/main/Kconfig.projbuild $(find ${IDF_PATH} -name Kconfig.profjbuild -printf '%p ')",
    "COMPONENT_SDKCONFIG_RENAMES": "$(find ${IDF_PATH}/components -name sdkconfig.rename -printf '%p ')",
    "IDF_CMAKE": "y",
    "IDF_TARGET": "esp32",
    "IDF_PATH": "${IDF_PATH}"
}
CONFIG_ENV_EOF

# create default sdkconfig
export IDF_TARGET=esp32
echo "Generating default sdkconfig"
python ${IDF_PATH}/tools/kconfig_new/confgen.py \
    --kconfig ${IDF_PATH}/Kconfig \
    --config ${RUN_DIR}/sdkconfig \
    --sdkconfig-rename ${IDF_PATH}/sdkconfig.rename \
    --defaults ${RUN_DIR}/sdkconfig.defaults \
    --env-file ${BUILD_DIR}/config.env \
    --output header ${BUILD_DIR}/config/sdkconfig.h \
    --output cmake ${BUILD_DIR}/config/sdkconfig.cmake \
    --output json ${BUILD_DIR}/config/sdkconfig.json \
    --output json_menus ${BUILD_DIR}/config/kconfig_menus.json \
    --output config ${RUN_DIR}/sdkconfig

cd ${BUILD_DIR} && cmake ${RUN_DIR} -G Ninja && ninja