const canvas = document.getElementById('videoCanvas');
const startButton = document.getElementById('startButton');
const fullscreenToggle = document.getElementById('fullscreenToggle');
const ctx = canvas.getContext('2d');
const maxTimeThreshold = 500; // milliseconds
const maxDistanceThreshold = 20 * 20;

let audioContext = null;
let videoDecoder = null;
let audioDecoder = null;
let ws;
let touchStartX = null;
let touchStartY = null;
let touchStartTime = null;
let touchActive = false;

// Handle start button for initial fullscreen and WebSocket setup
startButton.addEventListener('click', async () => {
    if (!audioContext || audioContext.state === 'closed') {
        audioContext = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });
        console.log('AudioContext sample rate:', audioContext.sampleRate);

        try {
            await audioContext.audioWorklet.addModule('audio-worklet-processor.js');
            const audioNode = new AudioWorkletNode(audioContext, 'audio-processor', {
                outputChannelCount: [2], // Explicitly specify two output channels
            });
            audioNode.connect(audioContext.destination);
            window.audioNode = audioNode;
            console.log('AudioWorklet loaded and connected');
        } catch (err) {
            console.error('Failed to load AudioWorklet:', err);
            return;
        }
    } else if (audioContext.state === 'suspended') {
        await audioContext.resume();
    }

    if (document.fullscreenEnabled) {
        try {
            await document.body.requestFullscreen();
            console.log('Fullscreen activated');
        } catch (err) {
            console.error('Error attempting fullscreen:', err);
            return;
        }
    } else {
        console.error('Fullscreen API is not supported by this browser.');
        return;
    }

    startButton.style.display = 'none';

    console.log("connecting to ws://localhost:8090/");
    ws = new WebSocket('ws://localhost:8090/');
    ws.binaryType = 'arraybuffer';

    ws.onopen = async function() {
        console.log('WebSocket connection opened');
        canvas.addEventListener('touchstart', function(event) {
            event.preventDefault();
            const touch = event.touches[0];
            touchActive = true;
            const rect = canvas.getBoundingClientRect();
            const x = touch.clientX - rect.left;
            const y = touch.clientY - rect.top;

            touchStartX = x;
            touchStartY = y;
            touchStartTime = performance.now();

            // Send touch start event to server
            const message = {
                type: 'touchstart',
                x: x,
                y: y
            };
            ws.send(JSON.stringify(message));
        });

        canvas.addEventListener('pointermove', function(event) {
            event.preventDefault();
            if (!event.isPrimary)
                return;
            if (touchActive)
            {
                const rect = canvas.getBoundingClientRect();
                const x = event.clientX - rect.left;
                const y = event.clientY - rect.top;

                const deltaTime = performance.now() - touchStartTime;
                const deltaX = x - touchStartX;
                const deltaY = y - touchStartY;
                const distanceSquared = deltaX * deltaX + deltaY * deltaY;

                if (deltaTime > maxTimeThreshold || distanceSquared > maxDistanceThreshold) {
                    const message = {
                        type: 'touchmove',
                        x: x,
                        y: y
                    };
                    ws.send(JSON.stringify(message));
                }
            }
            else
            {
                const rect = canvas.getBoundingClientRect();
                const x = event.clientX - rect.left;
                const y = event.clientY - rect.top;

                const message = {
                    type: 'touchmove',
                    x: x,
                    y: y
                };
                ws.send(JSON.stringify(message));
            }
        });

        canvas.addEventListener('touchend', function(event) {
            event.preventDefault();
            touchActive = false;
            const touch = event.changedTouches[0];
            const rect = canvas.getBoundingClientRect();
            const x = touch.clientX - rect.left;
            const y = touch.clientY - rect.top;
            const touchEndTime = performance.now();

            // Calculate time and movement differences
            const deltaTime = touchEndTime - touchStartTime;
            const deltaX = x - touchStartX;
            const deltaY = y - touchStartY;
            const distanceSquared = deltaX * deltaX + deltaY * deltaY;

            if (deltaTime <= maxTimeThreshold && distanceSquared <= maxDistanceThreshold) {
                // Consider it as a click at the touchStart position
                const message = {
                    type: 'touchend',
                    x: touchStartX,
                    y: touchStartY
                };
                ws.send(JSON.stringify(message));
            } else {
                // Send touchend event with current position
                const message = {
                    type: 'touchend',
                    x: x,
                    y: y
                };
                ws.send(JSON.stringify(message));
            }

            // Reset touch start variables
            touchStartX = null;
            touchStartY = null;
            touchStartTime = null;
        });
        canvas.addEventListener('wheel', function(event) {
            event.preventDefault();
            const deltaY = .02 * event.deltaY;
            const message = {
                type: 'scroll',
                deltaY: deltaY
            };
            ws.send(JSON.stringify(message));
        });
    };

    ws.onmessage = async function(event) {
        const data = event.data;
        const buffer = new Uint8Array(data);

        const messageType = buffer[0];

        if (messageType === 0x01) {
            const videoData = buffer.slice(1);

            if (!videoDecoder) {
                const videoConfig = {
                    codec: 'avc1.42E01E',
                    codedWidth: 1920,
                    codedHeight: 1080,
                    hardwareAcceleration: 'no-preference'
                };

                try {
                    const support = await VideoDecoder.isConfigSupported(videoConfig);
                    if (!support.supported) {
                        console.error('Configuration not supported:', support.config);
                        return;
                    }
                } catch (err) {
                    console.error('Error checking configuration:', err);
                    return;
                }

                try {
                    videoDecoder = new VideoDecoder({
                        output: frame => {
                            ctx.drawImage(frame, 0, 0, canvas.width, canvas.height);
                            frame.close();
                        },
                        error: err => {
                            console.error('Decoder error:', err);
                        }
                    });
                    console.log('VideoDecoder created');
                } catch (err) {
                    console.error('Error creating decoder:', err);
                    return;
                }
                try {
                    videoDecoder.configure(videoConfig);
                    console.log('VideoDecoder configured');
                } catch (err) {
                    console.error('Error configuring decoder:', err);
                    return;
                }
            }

            // Parse NAL units to determine frame type
            if (videoData.length < 5) {
                console.error('Video data too short to determine frame type');
                return;
            }

            const type = videoData[4] & 0x1F;
            const frameType = (type === 5) ? 'key' : 'delta';

            const chunk = new EncodedVideoChunk({
                type: frameType,
                timestamp: performance.now(),
                data: videoData
            });

            videoDecoder.decode(chunk);
        } else if (messageType === 0x02) {
            const opusData = buffer.slice(1);

            if (!audioDecoder){
                const audioConfig = {
                    codec: 'opus',
                    sampleRate: 48000,
                    numberOfChannels: 2,
                };
                try {
                    const support = await AudioDecoder.isConfigSupported(audioConfig);
                    if (!support.supported) {
                        console.error('Opus configuration not supported:', support.config);
                        return;
                    }
                } catch (err) {
                    console.error('Error checking Opus configuration:', err);
                    return;
                }
                try {
                    audioDecoder = new AudioDecoder({
                        output: (audioData) => {
                            // Create a Float32Array to hold the audio samples
                            const numChannels = audioData.numberOfChannels;
                            const numFrames = audioData.numberOfFrames;
                            const format = audioData.format; // Should be 'f32' (float32)

                            const audioBuffer = new Float32Array(numFrames * numChannels);

                            // Copy the data from the AudioData object
                            audioData.copyTo(audioBuffer, {
                                planeIndex: 0, // Only plane 0 for interleaved formats like f32
                                format: format,
                            });

                            audioData.close(); // Free the AudioData resource

                            // Send the extracted data to the AudioWorkletNode
                            if (window.audioNode && window.audioNode.port) {
                                window.audioNode.port.postMessage(audioBuffer.buffer, [audioBuffer.buffer]); // Transfer the ArrayBuffer
                            }
                        },
                        error: (err) => {
                            console.error('AudioDecoder error:', err);
                        },
                    });
                    audioDecoder.configure(audioConfig);
                } catch (err) {
                    console.error('Error creating AudioDecoder:', err);
                }

            }

            if (audioDecoder) {
                try {
                    const chunk = new EncodedAudioChunk({
                        type: 'key', // All Opus packets are treated as key frames
                        timestamp: performance.now(),
                        data: opusData,
                    });

                    audioDecoder.decode(chunk);
                } catch (err) {
                    console.error('Error decoding audio chunk:', err);
                }
            }
        } else {
            console.error('Unknown message type:', messageType);
        }
    };
});

// Add toggle fullscreen functionality to the floating button
fullscreenToggle.addEventListener('click', async () => {
    if (document.fullscreenElement) {
        try {
            await document.exitFullscreen();
            console.log('Fullscreen exited');
        } catch (err) {
            console.error('Error exiting fullscreen:', err);
        }
    } else if (document.fullscreenEnabled) {
        try {
            await document.body.requestFullscreen();
            console.log('Fullscreen activated');
        } catch (err) {
            console.error('Error entering fullscreen:', err);
        }
    }
});

let isDragging = false;
let offsetX, offsetY;

// Start dragging with touchstart
fullscreenToggle.addEventListener('touchstart', (event) => {
    isDragging = true;

    // Get the first touch point
    const touch = event.touches[0];

    // Calculate the offset between the touch point and the button's position
    const rect = fullscreenToggle.getBoundingClientRect();
    offsetX = touch.clientX - rect.left;
    offsetY = touch.clientY - rect.top;

    fullscreenToggle.style.cursor = 'grabbing';
});

fullscreenToggle.addEventListener('touchmove', (event) => {
    if (isDragging) {
        const touch = event.touches[0];

        // Update the button's position
        fullscreenToggle.style.left = `${touch.clientX - offsetX}px`;
        fullscreenToggle.style.top = `${touch.clientY - offsetY}px`;
    }
});

fullscreenToggle.addEventListener('touchend', () => {
    isDragging = false;
    fullscreenToggle.style.cursor = 'grab';
});

