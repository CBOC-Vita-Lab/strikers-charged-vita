# Controls

The native player defaults to **Modern**. Choose Modern or Classic in the
port's Settings screen; Triangle there opens its control guide. The tables
below follow `native/platform/sdk/input/np_control_table.h`. Source review
does not replace a complete current-build Vita input test.

## Modern

| Input | With the ball | Without the ball |
| --- | --- | --- |
| Cross | Pass | Switch player; hold for captain |
| Circle | Shoot; hold to charge | Slide tackle |
| Square | Lob pass | Big hit |
| Triangle | Use next item / super ability | Same |
| R | Chip ahead and run | No mapped action |
| R + Cross / Circle | Lob pass / chip shot | No mapped action |
| L | Same modifier as R; L + Circle chips | Switch player; hold for captain |

## Classic

| Input | With the ball | Without the ball |
| --- | --- | --- |
| Cross | Pass | Switch player; hold for captain |
| Square | Shoot; hold to charge | Slide tackle |
| Circle | Use next item / super ability | Same |
| Triangle | Deke | Big hit |
| L | Chip ahead and run | No mapped action |
| L + Cross / Square | Lob pass / chip shot | No mapped action |
| R | Deke toward the stick | Slide toward the stick |

## Both Layouts

Left stick moves. Right-stick flick dekes with the ball; it is not the same
as the D-pad, which dekes with the ball and slide-tackles without it.
Select cycles items. Start pauses/resumes.

Rear touch works only when enabled: left tap cycles items; right tap switches
players when defending and never passes while attacking.

## Mega Strike, Saves And Menus

- **Mega Strike:** with the captain, hold Circle (Modern) or Square (Classic),
  then press that button at each meter mark. Front touch can operate the
  meter when its touch option is enabled.
- **Mega Save:** aim with a stick and block with Cross or R. Front touch is
  available when the touch-save option is enabled.
- **Opponent's Mega Strike:** mash Square, Triangle or Select in Modern;
  Select in Classic. Rear-left taps and front swipes work when their
  respective touch options are enabled.
- **Menus:** Cross confirms; Circle goes back in Modern, Square in Classic.
  D-pad/sticks move and L/R changes page. Front-pointer touch is optional.

## Camera

Go to **Options > Visual**. Select level 5, then press its button again to
cycle **5 -> 6 -> 7 -> 5**. The label shows the selected level. Auto Zoom
still moves the camera with play. Levels 6 and 7 are closer views; the
computer team's visibility is widened so the closer camera does not simply
hide passing and item opportunities from it.

Existing saves keep their camera choice until changed. The Options cycle,
level 6 gameplay, rear touch and final-package settings persistence still
need current Vita acceptance. The touch mappings above are source-backed;
their complete hardware checks, motion, PS TV and external-controller
behavior remain unverified.
