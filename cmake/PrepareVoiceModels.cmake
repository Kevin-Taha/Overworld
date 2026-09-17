# Explicit developer action only. Never invoked on app startup or Settings Apply.
# OpenAI Whisper model/code: MIT, https://github.com/openai/whisper/blob/main/LICENSE
# Converted model: https://huggingface.co/ggerganov/whisper.cpp
# JFK inaugural speech: US federal-government/public-domain recording, distributed
# by the pinned MIT whisper.cpp source in samples/jfk.wav; fixture only, no user audio.
if(NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "Provide an explicit build-cache OUTPUT_DIR")
endif()
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(DOWNLOAD "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.en.bin"
    "${OUTPUT_DIR}/ggml-tiny.en.bin"
    EXPECTED_HASH SHA256=921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f
    TIMEOUT 60 INACTIVITY_TIMEOUT 15 TLS_VERIFY ON)
file(DOWNLOAD "https://raw.githubusercontent.com/ggml-org/whisper.cpp/a8d002cfd879315632a579e73f0148d06959de36/samples/jfk.wav"
    "${OUTPUT_DIR}/jfk.wav"
    EXPECTED_HASH SHA256=59dfb9a4acb36fe2a2affc14bacbee2920ff435cb13cc314a08c13f66ba7860e
    TIMEOUT 15 INACTIVITY_TIMEOUT 10 TLS_VERIFY ON)
