#include "draw_helper.h"
#include <algorithm>

namespace UTILS
{
    namespace UI
    {

        uint32_t node_color(uint32_t node_id)
        {
            uint8_t r = std::max((uint8_t)80, (uint8_t)((node_id >> 16) & 0xFF));
            uint8_t g = std::max((uint8_t)80, (uint8_t)((node_id >> 8) & 0xFF));
            uint8_t b = std::max((uint8_t)80, (uint8_t)(node_id & 0xFF));
            return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }

        uint32_t node_text_color(uint32_t node_id)
        {
            uint32_t bg = node_color(node_id);
            int lum = (int)(0.299 * ((bg >> 16) & 0xFF) + 0.587 * ((bg >> 8) & 0xFF) + 0.114 * (bg & 0xFF));
            return (lum > 128) ? (uint32_t)0x000000 : (uint32_t)0xFFFFFF;
        }

        void draw_scrollbar(LGFX_Sprite* canvas,
                            int x,
                            int y,
                            int width,
                            int height,
                            int total,
                            int visible,
                            int offset,
                            int min_thumb,
                            int track_color,
                            int thumb_color)
        {
            if (total <= visible)
                return;
            int thumb_h = std::max(min_thumb, height * visible / total);
            int thumb_y = y + (height - thumb_h) * offset / (total - visible);
            canvas->drawRect(x, y, width, height, track_color);
            canvas->fillRect(x, thumb_y, width, thumb_h, thumb_color);
        }

        static const char* routing_error_icon(uint8_t code)
        {
            switch (code)
            {
            case 1:
                return "RT"; // NO_ROUTE
            case 2:
                return "NK"; // GOT_NAK
            case 3:
                return "TO"; // TIMEOUT
            case 4:
                return "IF"; // NO_INTERFACE
            case 5:
                return "MX"; // MAX_RETRANSMIT
            case 6:
                return "CH"; // NO_CHANNEL
            case 7:
                return "SZ"; // TOO_LARGE
            case 8:
                return "NR"; // NO_RESPONSE
            case 9:
                return "DC"; // DUTY_CYCLE_LIMIT
            case 32:
                return "RQ"; // BAD_REQUEST
            case 33:
                return "AU"; // NOT_AUTHORIZED
            case 34:
                return "PK"; // PKI_FAILED
            case 35:
                return "KY"; // PKI_UNKNOWN_PUBKEY
            default:
                return "XX";
            }
        }

        const char* routing_error_name(uint8_t code)
        {
            switch (code)
            {
            case 0:
                return nullptr;
            case 1:
                return "NO_ROUTE";
            case 2:
                return "GOT_NAK";
            case 3:
                return "TIMEOUT";
            case 4:
                return "NO_INTERFACE";
            case 5:
                return "MAX_RETRANSMIT";
            case 6:
                return "NO_CHANNEL";
            case 7:
                return "TOO_LARGE";
            case 8:
                return "NO_RESPONSE";
            case 9:
                return "DUTY_CYCLE_LIMIT";
            case 32:
                return "BAD_REQUEST";
            case 33:
                return "NOT_AUTHORIZED";
            case 34:
                return "PKI_FAILED";
            case 35:
                return "PKI_UNKNOWN_PUBKEY";
            default:
                return "UNKNOWN";
            }
        }

        StatusInfo message_status_info(int status, uint8_t error_code)
        {
            switch (status)
            {
            case 0:
                return {TFT_DARKGREY, ">"};
            case 1:
                return {TFT_WHITE, ">>"};
            case 2:
                return {TFT_GREEN, "V"};
            case 3:
                return {TFT_RED, routing_error_icon(error_code)};
            case 4:
                return {TFT_GREEN, "VV"};
            case 5:
                return {TFT_RED, routing_error_icon(error_code)};
            default:
                return {TFT_WHITE, "?"};
            }
        }

    } // namespace UI
} // namespace UTILS
