import re

with open('src/core/camera_pipeline_manager.cpp', 'r', encoding='utf-8') as f:
    text = f.read()

# Add iostream if missing
if '<iostream>' not in text:
    text = "#include <iostream>\n" + text

text = re.sub(r'~PipelineContext\(\)\s*\{', '~PipelineContext() {\n        std::cout << "[CRASH-TRACE] ~PipelineContext() started" << std::endl;', text)
text = text.replace('if (native_reader_thread) {', 'std::cout << "[CRASH-TRACE] 2. Stopping NativeReaderWorker" << std::endl;\n        if (native_reader_thread) {')
text = text.replace('if (buffer_pipeline) {', 'std::cout << "[CRASH-TRACE] 3. Stopping BufferPipeline" << std::endl;\n        if (buffer_pipeline) {')
text = text.replace('if (continuous_recorder) {', 'std::cout << "[CRASH-TRACE] 3b. Stopping ContinuousRecorder" << std::endl;\n        if (continuous_recorder) {')
text = text.replace('if (mediamtx_publisher) {', 'std::cout << "[CRASH-TRACE] 4. Stopping MediaMTX Publisher" << std::endl;\n        if (mediamtx_publisher) {')
text = text.replace('if (ai_process) {', 'std::cout << "[CRASH-TRACE] 5. Stopping AI Process" << std::endl;\n        if (ai_process) {')
text = text.replace('if (process) {', 'std::cout << "[CRASH-TRACE] 6. Stopping H264 Process" << std::endl;\n        if (process) {')
text = text.replace('    } // ~PipelineContext() end / struct PipelineContext', '        std::cout << "[CRASH-TRACE] ~PipelineContext() COMPLETED SUCCESSFULLY!" << std::endl;\n    }')
# Replace the end brace of PipelineContext to print completion
import textwrap
text = text.replace('        //    will be cleaned up by their own destructors automatically.\n    }', '        //    will be cleaned up by their own destructors automatically.\n        std::cout << "[CRASH-TRACE] ~PipelineContext() COMPLETED SUCCESSFULLY!" << std::endl;\n    }')


with open('src/core/camera_pipeline_manager.cpp', 'w', encoding='utf-8') as f:
    f.write(text)
