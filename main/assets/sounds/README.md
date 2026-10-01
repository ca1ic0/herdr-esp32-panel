# Herdr notification sounds

Source: [`herdrdev/herdr` at `d6b40d4edd550ccea081f089605a64314f8c8b27`](https://github.com/herdrdev/herdr/tree/d6b40d4edd550ccea081f089605a64314f8c8b27/assets/sounds).
The upstream repository is Apache-2.0 licensed; its license text is copied to `LICENSE`. Confirm asset-specific provenance before distributing a product image.

| File | SHA-256 |
| --- | --- |
| `done.mp3` | `cb9c93492c808e33946edac2ab5915758267e65914bc4b794a5ca6dc6e655741` |
| `request.mp3` | `44c077ce28a0a48cec29da204beb627fd17140e5bde1f92dbaecf3d478bdda6f` |
| `done.pcm` | `d9ed7df89d2c2034cf081e167de631cc999a71394f7a393cefb19644bb6462f8` |
| `request.pcm` | `6c4a8db0929dbeb5acfd9f47d98c332fbdc93546f3a5dcd981b1d5287bae5f97` |

PCM is 16 kHz, signed 16-bit, interleaved stereo, little-endian. Reproduce with:

```sh
ffmpeg -i done.mp3 -ar 16000 -ac 2 -f s16le done.pcm
ffmpeg -i request.mp3 -ar 16000 -ac 2 -f s16le request.pcm
```

Only PCM files are embedded in firmware. The MP3 files are retained as pinned source assets for comparison and future conversion.
