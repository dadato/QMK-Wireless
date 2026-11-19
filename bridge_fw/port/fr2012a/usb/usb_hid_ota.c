#include "usb_hid_ota.h"
#include "ota.h"
#include "os_mem.h"
#include "sys_utils.h"
#include "usb_hid.h"
#include "driver_flash.h"
#include "crc32.h"
#include "os_timer.h"
#include "os_task.h"
#include "driver_uart.h"
#include "co_log.h"
#undef LOG_LOCAL_LEVEL
#define LOG_LOCAL_LEVEL        (LOG_LEVEL_NONE)
static const char *app_tag = "usb_ota";

extern  uint8_t first_loop;
static uint8_t *ota_recving_buffer = NULL;
static uint16_t at_data_idx;
uint16_t usb_hid_ota_task_id;




static int usb_hid_ota_task_func(os_event_t *event)
{
    switch(event->event_id)
    {
        case USB_HID_OTA_REC:
            uart_putc_noint_no_wait(UART0 ,'R');
            usb_hid_otas_recv_data((uint8_t *)event->param);
            break;
        case USB_TASK_TEST:
            uart_putc_noint_no_wait(UART0 ,'U');
            break;
    }

    return EVT_CONSUMED;
}
void usb_hid_ota_task_init(void )
{
    usb_hid_ota_task_id = os_task_create(usb_hid_ota_task_func);
}
static uint32_t app_otas_get_storage_address(void)
{
    struct image_info_t *image_info = (struct image_info_t *)(QSPI0_DAC_ADDRESS+A_ZONE_INFO_OFFSET);
    return (image_info->jump_table.image_size + A_ZONE_IMAGE_OFFSET);  //partA, then return partB flash Addr
}

void usb_hid_otas_recv_data(uint8_t *p_data)
{
    struct app_ota_cmd_hdr_t *cmd_hdr = (struct app_ota_cmd_hdr_t *)p_data;
    struct app_ota_rsp_hdr_t *rsp_hdr;
    uint16_t rsp_data_len = (OTA_HDR_OPCODE_LEN+OTA_HDR_LENGTH_LEN+OTA_HDR_RESULT_LEN);


    if(first_loop)
    {
        first_loop = false;
        app_set_ota_state(1);

        if(ota_recving_buffer == NULL) {
            ota_recving_buffer = os_malloc(512);
        }
#ifdef OTA_CRC_CHECK
        //printf("ota_start1111\r\n");
        ota_start();
#endif
    }

    LOG_INFO(app_tag,"app_otas_recv_data[%d]: %d\r\n",at_data_idx, cmd_hdr->cmd.write_data.length);
    show_reg(p_data,sizeof(struct app_ota_cmd_hdr_t),LOG_LOCAL_LEVEL);
#ifdef OTA_CRC_CHECK
    os_timer_stop(&os_timer_ota);
    os_timer_start(&os_timer_ota, OTA_TIMEOUT, 0);
#endif
    at_data_idx++;
	ota_change_flash_pin();
    switch(cmd_hdr->opcode){
        case OTA_CMD_GET_STR_BASE:
            at_data_idx = 0;
            
            rsp_data_len += sizeof(struct storage_baseaddr);
            break;
		case OTA_CMD_PAGE_ERASE:
            rsp_data_len += sizeof(struct page_erase_rsp);
            break;
        case OTA_CMD_WRITE_DATA:
            rsp_data_len += sizeof(struct write_data_rsp);
            break;
    }

    struct otas_send_rsp *req =os_zalloc(sizeof(struct otas_send_rsp)+64); //os_malloc(sizeof(struct otas_send_rsp) + rsp_data_len);
    uint16_t base_length;


    req->length = rsp_data_len;
    rsp_hdr = (struct app_ota_rsp_hdr_t *)&req->buffer[0];
    rsp_hdr->result = OTA_RSP_SUCCESS;
    rsp_hdr->org_opcode = cmd_hdr->opcode;
    rsp_hdr->length = rsp_data_len - (OTA_HDR_OPCODE_LEN+OTA_HDR_LENGTH_LEN+OTA_HDR_RESULT_LEN);

    switch(cmd_hdr->opcode){
        case OTA_CMD_GET_STR_BASE:
            rsp_hdr->rsp.baseaddr.baseaddr = app_otas_get_storage_address();
#ifdef OTA_CRC_CHECK
            ota_addr_check = rsp_hdr->rsp.baseaddr.baseaddr;
#endif
            break;
        case OTA_CMD_PAGE_ERASE:
        {
            rsp_hdr->rsp.page_erase.base_address = cmd_hdr->cmd.page_erase.base_address;
#if 1
            ///co_printf("cur_code_addr:%x\r\n",new_bin_base);
            if(rsp_hdr->rsp.page_erase.base_address < app_otas_get_storage_address())
            {
                LOG_INFO(app_tag,"Err:%x\r\n",rsp_hdr->rsp.page_erase.base_address );
                break;
            }
#endif
			LOG_INFO(app_tag,"erase addr:0x%x\r\n",rsp_hdr->rsp.page_erase.base_address);
            app_otas_erase(rsp_hdr->rsp.page_erase.base_address);
			LOG_INFO(app_tag,"erase end \r\n");
        }
        break;
		case OTA_CMD_WRITE_DATA:
        {
            rsp_hdr->rsp.write_data.base_address = cmd_hdr->cmd.write_data.base_address;
            rsp_hdr->rsp.write_data.length = cmd_hdr->cmd.write_data.length;
//write user data.
#ifdef OTA_CRC_CHECK
            if((rsp_hdr->rsp.write_data.base_address !=(ota_addr_check + ota_addr_check_len)) &&
                (rsp_hdr->rsp.write_data.base_address !=ota_addr_check)){//for OTA write addr error  no req
                LOG_INFO(app_tag,"rsp_hdr->rsp.write_data.base_address = %x\r\nota_addr_check=%x,\r\nSUM=%x\r\n",rsp_hdr->rsp.write_data.base_address,ota_addr_check,rsp_hdr->rsp.write_data.base_address +55);
                os_free(req);
                ota_stop(OTA_ADDR_ERROR);
                return;
            }else{
                ota_addr_check = rsp_hdr->rsp.write_data.base_address;
                ota_addr_check_len = rsp_hdr->rsp.write_data.length;
            }
#endif
            app_otas_save_data(rsp_hdr->rsp.write_data.base_address,
                               p_data + (OTA_HDR_OPCODE_LEN+OTA_HDR_LENGTH_LEN)+sizeof(struct write_data_cmd),
                               rsp_hdr->rsp.write_data.length);
        }
        break;
		case OTA_CMD_REBOOT:
            {
                uint32_t check_crc = 0;
                LOG_INFO(app_tag,"REBOOT\r\n\r\n");
                uint32_t image_offset, image_length;
                struct image_info_t image_info_tmp = {0};
                flash_read(app_otas_get_storage_address(), sizeof(struct jump_table_t), (uint8_t *)&image_info_tmp);
                image_offset = app_otas_get_storage_address() + B_ZONE_IMAGE_OFFSET;
                image_length = image_info_tmp.jump_table.image_size - B_ZONE_IMAGE_OFFSET;
                
                image_info_tmp.compressed_info.compressed_label = 0xFFFFFFFF;
                image_info_tmp.compressed_info.compressed_image_crc = 0;

                image_info_tmp.crc.image_crc = crc32(0xffffffff, (const uint8_t *)(QSPI0_DAC_ADDRESS + image_offset), image_length);
                LOG_INFO(app_tag,"CRC:0x%x   addr:0x%x   len=%d\r\n",image_info_tmp.crc.image_crc,QSPI0_DAC_ADDRESS + image_offset,image_length);
                image_info_tmp.crc.jump_table_crc 
                    = crc32(0xffffffff, (const uint8_t *)(&image_info_tmp),(uint32_t)&(((struct jump_table_t *)0)->checkword)); 

                image_info_tmp.image_storage_offset = image_offset;
                image_info_tmp.image_length = image_length;
                
                image_info_tmp.flash_protect_mask = 0x7C;//0xFFFFFFFF;
                image_info_tmp.image_valid = JUMP_TABLE_OTA_IMAGE_VALID;
                
#ifdef OTA_CRC_CHECK
				//printf("rec: crc = 0x%x, fw_len = %d",cmd_hdr->cmd.fir_crc_data.CRC32_data,cmd_hdr->cmd.fir_crc_data.firmware_length);
				check_crc = crc32(0xffffffff, (const uint8_t *)(QSPI0_DAC_ADDRESS + app_otas_get_storage_address()), cmd_hdr->cmd.fir_crc_data.firmware_length);
				LOG_INFO(app_tag,"check_crc = 0x%x  rec_crc = 0x%x\r\n",check_crc,cmd_hdr->cmd.fir_crc_data.CRC32_data);
				rsp_hdr->rsp.firmware_check.CRC32_data = check_crc;
                if(check_crc == cmd_hdr->cmd.fir_crc_data.CRC32_data ){
#endif      
                app_otas_erase(B_ZONE_INFO_OFFSET);
                app_otas_save_data(B_ZONE_INFO_OFFSET,(uint8_t *)&image_info_tmp,sizeof(struct image_info_t));
/*
                co_printf("image_storage_offset:0x%x,image_length:0x%x\r\n\r\n",image_info_tmp.image_storage_offset,image_info_tmp.image_length);
                uint32_t crc = crc32(0xffffffff, (const uint8_t *)(QSPI0_DAC_ADDRESS + B_ZONE_INFO_OFFSET),
                        (uint32_t)&(((struct jump_table_t *)0)->checkword));
                co_printf("check1,crc:0x%x,jump_table_crc:0x%x\r\n\r\n",crc,image_info_tmp.crc.jump_table_crc);
                crc = crc32(0xffffffff, (const uint8_t *)(QSPI0_DAC_ADDRESS + image_offset), image_length);
                co_printf("check2,crc:0x%x,image_crc:0x%x\r\n\r\n",crc,image_info_tmp.crc.image_crc);
                show_reg((uint8_t *)(QSPI0_DAC_ADDRESS + B_ZONE_INFO_OFFSET),80,1);
*/
#ifdef OTA_CRC_CHECK    
                }
                else{
					rsp_hdr->result = OTA_RSP_ERROR;
					usb_hid_send_custom_report1(req->buffer);
                    co_printf("crc32 check fail\r\n\r\n");
                    os_free(req);
                    ota_stop(OTA_CHECK_FAIL);
					co_delay_100us(100);
                    platform_reset(0);
                }
#endif  
            }
			usb_hid_send_custom_report1(req->buffer);
            app_set_ota_state(0);
            //uart_finish_transfers(UART1_BASE);
			co_delay_100us(100);
            platform_reset(0);
            break;
        default:
            rsp_hdr->result = OTA_RSP_UNKNOWN_CMD;
            break;
    } 
    usb_hid_send_custom_report1(req->buffer);
	ota_recover_flash_pin();
    os_free(req);
}