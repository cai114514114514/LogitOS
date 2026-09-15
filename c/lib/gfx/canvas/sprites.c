#include "openlogit_canvas.h"

int ol_cmd_nine_slice(struct ol_list *list, struct ol_image *image, struct gfx_rect source,
                      struct gfx_rect destination, struct ol_borders borders,
                      const struct gfx_rect *clip, int opacity, int bilinear)
{
    struct ol_surface_view view;
    if (ol_image_view(image, &view) != OL_OK || view.format != OL_FORMAT_RGBA8_STRAIGHT ||
        source.x < 0 || source.y < 0 || source.w < 1 || source.h < 1 ||
        source.w > (int)view.width || source.h > (int)view.height ||
        source.x > (int)view.width - source.w || source.y > (int)view.height - source.h ||
        borders.left < 0 || borders.top < 0 || borders.right < 0 || borders.bottom < 0 ||
        borders.left >= source.w || borders.right >= source.w - borders.left ||
        borders.top >= source.h || borders.bottom >= source.h - borders.top ||
        destination.w < borders.left + borders.right ||
        destination.h < borders.top + borders.bottom || destination.x < -32768 ||
        destination.y < -32768 || destination.w > 32768 || destination.h > 32768 ||
        destination.x > 32768 - destination.w || destination.y > 32768 - destination.h)
        return ol_list_invalidate(list, OL_ARGUMENT);
    int sx[] = {source.x, source.x + borders.left, source.x + source.w - borders.right,
                source.x + source.w};
    int sy[] = {source.y, source.y + borders.top, source.y + source.h - borders.bottom,
                source.y + source.h};
    int dx[] = {destination.x, destination.x + borders.left,
                destination.x + destination.w - borders.right, destination.x + destination.w};
    int dy[] = {destination.y, destination.y + borders.top,
                destination.y + destination.h - borders.bottom, destination.y + destination.h};
    for (unsigned row = 0; row < 3; row++) {
        for (unsigned col = 0; col < 3; col++) {
            int sw = sx[col + 1] - sx[col], sh = sy[row + 1] - sy[row];
            int dw = dx[col + 1] - dx[col], dh = dy[row + 1] - dy[row];
            if (!sw || !sh || !dw || !dh)
                continue;
            int status = ol_cmd_image_region(
                list, image, (struct gfx_rect){sx[col], sy[row], sw, sh},
                (struct gfx_rect){dx[col], dy[row], dw, dh}, clip, opacity, bilinear);
            if (status != OL_OK)
                return status;
        }
    }
    return OL_OK;
}
