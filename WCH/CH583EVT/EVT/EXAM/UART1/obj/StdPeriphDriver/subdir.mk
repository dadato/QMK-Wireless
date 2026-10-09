################################################################################
# MRS Version: 2.5.0
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_clk.c \
c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_flash.c \
c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_gpio.c \
c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_i2c.c \
c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_pwr.c \
c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_spi1.c \
c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_sys.c \
c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_uart0.c \
c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_uart1.c \
c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_usb2dev.c \
c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_usb2hostBase.c \
c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_usb2hostClass.c 

C_DEPS += \
./StdPeriphDriver/CH58x_clk.d \
./StdPeriphDriver/CH58x_flash.d \
./StdPeriphDriver/CH58x_gpio.d \
./StdPeriphDriver/CH58x_i2c.d \
./StdPeriphDriver/CH58x_pwr.d \
./StdPeriphDriver/CH58x_spi1.d \
./StdPeriphDriver/CH58x_sys.d \
./StdPeriphDriver/CH58x_uart0.d \
./StdPeriphDriver/CH58x_uart1.d \
./StdPeriphDriver/CH58x_usb2dev.d \
./StdPeriphDriver/CH58x_usb2hostBase.d \
./StdPeriphDriver/CH58x_usb2hostClass.d 

OBJS += \
./StdPeriphDriver/CH58x_clk.o \
./StdPeriphDriver/CH58x_flash.o \
./StdPeriphDriver/CH58x_gpio.o \
./StdPeriphDriver/CH58x_i2c.o \
./StdPeriphDriver/CH58x_pwr.o \
./StdPeriphDriver/CH58x_spi1.o \
./StdPeriphDriver/CH58x_sys.o \
./StdPeriphDriver/CH58x_uart0.o \
./StdPeriphDriver/CH58x_uart1.o \
./StdPeriphDriver/CH58x_usb2dev.o \
./StdPeriphDriver/CH58x_usb2hostBase.o \
./StdPeriphDriver/CH58x_usb2hostClass.o 

DIR_OBJS += \
./StdPeriphDriver/*.o \

DIR_DEPS += \
./StdPeriphDriver/*.d \

DIR_EXPANDS += \
./StdPeriphDriver/*.234r.expand \


# Each subdirectory must supply rules for building sources it contributes
StdPeriphDriver/CH58x_clk.o: c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_clk.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
StdPeriphDriver/CH58x_flash.o: c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_flash.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
StdPeriphDriver/CH58x_gpio.o: c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_gpio.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
StdPeriphDriver/CH58x_i2c.o: c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_i2c.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
StdPeriphDriver/CH58x_pwr.o: c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_pwr.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
StdPeriphDriver/CH58x_spi1.o: c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_spi1.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
StdPeriphDriver/CH58x_sys.o: c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_sys.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
StdPeriphDriver/CH58x_uart0.o: c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_uart0.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
StdPeriphDriver/CH58x_uart1.o: c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_uart1.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
StdPeriphDriver/CH58x_usb2dev.o: c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_usb2dev.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
StdPeriphDriver/CH58x_usb2hostBase.o: c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_usb2hostBase.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"
StdPeriphDriver/CH58x_usb2hostClass.o: c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/CH58x_usb2hostClass.c
	@	riscv-none-embed-gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -msmall-data-limit=8 -mno-save-restore -fmax-errors=20 -Os -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -fno-common -g -DDEBUG=1 -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/StdPeriphDriver/inc" -I"c:/Users/Administrator/Desktop/qmk/qmk_firmware/wireless/WCH/CH583EVT/EVT/EXAM/SRC/RVMSIS" -std=gnu99 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@)" -c -o "$@" "$<"

