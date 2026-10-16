################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../APP/hidkbd.c \
../APP/hidkbd_main.c 

C_DEPS += \
./APP/hidkbd.d \
./APP/hidkbd_main.d 

OBJS += \
./APP/hidkbd.o \
./APP/hidkbd_main.o 

DIR_OBJS += \
./APP/*.o \

DIR_DEPS += \
./APP/*.d \

DIR_EXPANDS += \
./APP/*.234r.expand \


# Each subdirectory must supply rules for building sources it contributes
APP/%.o: ../APP/%.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/Startup" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/BLE/HID_Keyboard/APP/include" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/BLE/HID_Keyboard/Profile/include" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/BLE/HAL/include" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/Ld" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/BLE/LIB" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

