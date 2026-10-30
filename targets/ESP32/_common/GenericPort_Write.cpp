//
// Copyright (c) .NET Foundation and Contributors
// See LICENSE file in the project root for full license information.
//

#include <esp32_idf.h>

#include <nanoCLR_Types.h>
#include <nanoCLR_Runtime.h>

#if CONFIG_NF_WP_TRANSPORT_USB_CDC && CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
// ROM flag (esp32s3.rom.ld): ROM printf output to the console UART. Output to USB-Serial/JTAG is
// controlled separately by g_usb_print.
extern "C" bool g_uart_print;
#endif

uint32_t GenericPort_Write(int portNum, const char *data, size_t size)
{
    // FIXME
    // It's only enabled if the C# programm is in running mode and no source level debugger (C# debugger)
    // is attached. In all other conditions it's better to be quiet.
    // If the debugger is attached we don't output the data because the debugger use the same serial link.
    // Maybe later it can also redirect output to the ESP32 app trace
    // which is redirected to JTAG/openocd interface
    (void)portNum;

#if CONFIG_NF_WP_TRANSPORT_USB_CDC
    // USB-only Wire Protocol means UART0 (COM1) belongs to the application; on Seadragon it is the
    // RS-485 Modbus port. ets_printf writes straight into the ROM console UART (UART0) TX FIFO,
    // bypassing the UART driver, so Debug.WriteLine text and exception dumps were transmitted on
    // the Modbus pins whenever no debugger was attached.
#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
    // Keep the ROM's USB-Serial/JTAG copy (serial monitor on the USB port) and stop its UART copy.
    g_uart_print = false;
#else
    (void)data;
    return (uint32_t)size;
#endif
#endif

    if (g_CLR_RT_ExecutionEngine.m_iDebugger_Conditions == CLR_RT_ExecutionEngine::c_fDebugger_StateProgramRunning)
    {
        char *p = (char *)data;
        int counter = 0;

        // send characters directly to the trace port
        while (*p != '\0' || counter < (int)size)
        {
            ets_printf("%c", *p++);
            counter++;
        }
        return counter;
    }
    else
    {
        return (uint32_t)size;
    }
}
