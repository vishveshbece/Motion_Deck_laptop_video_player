# Motion Deck video player

This local web app plays a video selected from the laptop and accepts acceleration samples from the SiWG917 board. The video file stays in the browser; the server only relays small JSON readings to the open player page.

## Run on the laptop

1. Install Node.js if it is not already installed.
2. Open a terminal in this `video_player` directory and run `npm start` (or `node server.js`).
3. Open `http://localhost:8080` in the laptop browser and choose a local video file.
4. Use the LAN IP printed by the server in `app.c` as `HTTP_SERVER_IP`. The firmware uses port `8080` and POSTs samples to `/coordinates`.
5. Allow Node.js through Windows Firewall on the private Wi-Fi network if prompted. Keep the laptop and board on the same Wi-Fi network.
6. Click Play once, keep the board steady, and press **Set neutral position**. Adjust flick sensitivity if needed.

## Motion mapping

- Tilt up/down relative to the calibrated neutral angle: volume up/down.
- Flick along X: seek forward/back by 10 seconds.
- Flick along Z: play/pause.

The axis signs depend on how the board is held. The live X/Y/Z readings, sensitivity slider, calibration button, mouse controls, and keyboard controls help tune the setup. The first version is a prototype; gesture thresholds may need adjustment for the way you orient and move the board.

The server has no third-party packages. It binds to all laptop network interfaces on port 8080, serves the player at `/`, receives `POST /coordinates`, and streams samples to the browser using Server-Sent Events.
