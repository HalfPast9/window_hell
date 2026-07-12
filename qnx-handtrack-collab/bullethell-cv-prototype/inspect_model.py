import tensorflow as tf

interpreter = tf.lite.Interpreter(model_path="hand_landmark_full.tflite")
interpreter.allocate_tensors()

print("--- INPUT ---")
print(interpreter.get_input_details()[0])
for i, d in enumerate(interpreter.get_output_details()):
    print(i, d['name'], d['shape'], d['dtype'])