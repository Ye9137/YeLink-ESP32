# Documentation artwork

- `yelink-hero.png`: generated with the built-in imagegen tool on 2026-10-16. Concept illustration, not a photograph, wiring reference or firmware screenshot.
- `wiring.svg`: project-authored logical wiring diagram. GPIO mapping follows `main/main.c`; the layout does not show physical pin positions.

## Hero generation prompt

```text
Use case: product-mockup
Asset type: open-source GitHub project hero image, wide landscape 16:9.
Primary request: A polished editorial 3D concept illustration for YeLink ESP32, a classic ESP32 development board connected to a small portrait 128x160 ST7735 TFT display. The display shows a dark router status UI with cyan UP and DOWN labels and simple green mini bars, no numerical performance claims. Show the bare black ESP32 dev board with a silver radio module and a USB power lead, the separate red TFT breakout on a clean dark navy desktop, a few neatly curved jumper wires. Composition: hardware on right, ample negative space on left for exact title and subtitle. A restrained cyan wireless arc floating above the hardware communicates Wi-Fi sharing. Soft studio light, crisp high quality product concept, realistic components but explicitly conceptual not a wiring diagram.
Text verbatim: large 'YeLink ESP32'; subtitle 'Wi-Fi Sharing · Tiny Display'; small footer 'CONCEPT RENDER'.
Avoid cats, third-party characters, people, brand logos other than project title, fake speed claims, batteries, screens on the ESP32 board itself. Keep typography readable, refined and minimal.
```
