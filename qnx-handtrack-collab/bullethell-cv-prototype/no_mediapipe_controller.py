import cv2
import numpy as np
import math
import time
import tensorflow as tf

# --- 1. DUAL TFLITE SETUP ---
# Landmark Model
landmark_interpreter = tf.lite.Interpreter(model_path="hand_landmark_full.tflite")
landmark_interpreter.allocate_tensors()
LANDMARK_IN = landmark_interpreter.get_input_details()[0]['index']
LANDMARK_OUT = landmark_interpreter.get_output_details()[0]['index'] 
LANDMARK_PRESENCE = landmark_interpreter.get_output_details()[1]['index'] 
LANDMARK_SIZE = 224 

# Palm Detection Model
palm_interpreter = tf.lite.Interpreter(model_path="palm_detection_full.tflite")
palm_interpreter.allocate_tensors()
PALM_IN = palm_interpreter.get_input_details()[0]['index']
PALM_OUT_REG = palm_interpreter.get_output_details()[0]['index'] # Box offsets
PALM_OUT_SCO = palm_interpreter.get_output_details()[1]['index'] # Box scores
PALM_SIZE = 192 # palm_detection_full uses 192x192. (If using lite, it's 128)

# --- 2. TRACKING VARIABLES ---
DEADZONE = 0.05
CALIBRATION_DURATION = 8.0
TRACK_LOST_FRAMES = 5
PRESENCE_THRESHOLD = 0.5

calibration_start_time = None
centers = {"Left": None, "Right": None}
track_state = {
    "Left": {"box": None, "lost_count": 0},
    "Right": {"box": None, "lost_count": 0},
}

# --- 3. PURE PYTHON AI PALM DECODER ---
def detect_palm_center(frame_crop):
    """Bypasses library overhead to extract a rough bounding box directly from the TFLite grid."""
    h_orig, w_orig = frame_crop.shape[:2]
    rgb = cv2.cvtColor(frame_crop, cv2.COLOR_BGR2RGB)
    resized = cv2.resize(rgb, (PALM_SIZE, PALM_SIZE))
    
    # Palm models generally expect input scaled between -1.0 and 1.0 or 0 to 1.
    input_data = np.expand_dims(resized / 255.0, axis=0).astype(np.float32)
    palm_interpreter.set_tensor(PALM_IN, input_data)
    palm_interpreter.invoke()
    
    regressors = palm_interpreter.get_tensor(PALM_OUT_REG) 
    scores = palm_interpreter.get_tensor(PALM_OUT_SCO)     
    
    # Find the single best anchor box out of the 2016 grid cells
    max_idx = np.argmax(scores[0])
    raw_score = np.clip(scores[0, max_idx, 0], -50, 50)
    presence = 1.0 / (1.0 + np.exp(-raw_score)) # Sigmoid activation
    
    if presence < PRESENCE_THRESHOLD:
        return None
        
    # Spatial Index Math: Figure out where that grid cell physically is on the screen
    anchor_count = scores.shape[1]
    grid1_anchors = 1152 if anchor_count == 2016 else 512 # Supports full (2016) or lite (896) models
    grid1_size = 24 if anchor_count == 2016 else 16
    grid2_size = 12 if anchor_count == 2016 else 8
    
    if max_idx < grid1_anchors:
        grid_idx = max_idx // 2
        y, x = grid_idx // grid1_size, grid_idx % grid1_size
        anchor_cx, anchor_cy = (x + 0.5) / float(grid1_size), (y + 0.5) / float(grid1_size)
    else:
        rem = max_idx - grid1_anchors
        grid_idx = rem // 6
        y, x = grid_idx // grid2_size, grid_idx % grid2_size
        anchor_cx, anchor_cy = (x + 0.5) / float(grid2_size), (y + 0.5) / float(grid2_size)
        
    # Apply AI offsets to get the exact center
    dx = regressors[0, max_idx, 0] / float(PALM_SIZE)
    dy = regressors[0, max_idx, 1] / float(PALM_SIZE)
    w_raw = regressors[0, max_idx, 2] / float(PALM_SIZE)
    h_raw = regressors[0, max_idx, 3] / float(PALM_SIZE)
    
    # Map back to the original screen dimensions
    center_x = int((anchor_cx + dx) * w_orig)
    center_y = int((anchor_cy + dy) * h_orig)
    size = max(int(w_raw * w_orig), int(h_raw * h_orig), 100) * 1.5 
    
    return center_x, center_y, int(size)

# --- 4. FLUID TRACKING PIPELINE ---
def get_padded_crop(frame, cx, cy, size):
    """Pads edges with black to prevent the hand box from getting stuck on screen edges."""
    h, w = frame.shape[:2]
    half = size // 2
    x1, y1 = cx - half, cy - half
    x2, y2 = cx + half, cy + half
    
    pad_left, pad_top = max(0, -x1), max(0, -y1)
    pad_right, pad_bottom = max(0, x2 - w), max(0, y2 - h)
    
    cx1, cy1 = max(0, x1), max(0, y1)
    cx2, cy2 = min(w, x2), min(h, y2)
    crop = frame[cy1:cy2, cx1:cx2]
    
    if pad_left > 0 or pad_top > 0 or pad_right > 0 or pad_bottom > 0:
        crop = cv2.copyMakeBorder(crop, pad_top, pad_bottom, pad_left, pad_right, 
                                  cv2.BORDER_CONSTANT, value=[0, 0, 0])
    return crop, x1, y1

def run_landmark_model(frame, cx, cy, size):
    crop, x1, y1 = get_padded_crop(frame, cx, cy, size)
    crop_rgb = cv2.cvtColor(crop, cv2.COLOR_BGR2RGB)
    crop_resized = cv2.resize(crop_rgb, (LANDMARK_SIZE, LANDMARK_SIZE))
    input_data = np.expand_dims(crop_resized / 255.0, axis=0).astype(np.float32)
    
    landmark_interpreter.set_tensor(LANDMARK_IN, input_data)
    landmark_interpreter.invoke()
    
    presence_raw = landmark_interpreter.get_tensor(LANDMARK_PRESENCE)[0][0]
    presence = 1 / (1 + np.exp(-presence_raw)) if abs(presence_raw) > 1.0 else float(presence_raw)
    
    raw_output = landmark_interpreter.get_tensor(LANDMARK_OUT)[0]
    landmarks = [(int(x1 + (raw_output[i*3]/LANDMARK_SIZE)*size),
                  int(y1 + (raw_output[i*3+1]/LANDMARK_SIZE)*size)) for i in range(21)]
    return presence, landmarks, (x1, y1, x1+size, y1+size)

def process_hand_region(frame, side, frame_w, frame_h):
    state = track_state[side]
    
    # Step A: Find the hand (either from tracking history, or from a fresh Palm AI scan)
    if state["box"] is not None and state["lost_count"] < TRACK_LOST_FRAMES:
        cx, cy, size = state["box"]              
    else:
        # Split screen search: Left hand scans left half, Right hand scans right half
        search_crop = frame[:, :frame_w // 2] if side == "Left" else frame[:, frame_w // 2:]
        offset_x = 0 if side == "Left" else frame_w // 2
        
        detected = detect_palm_center(search_crop)
        if detected is None: return None
        
        cx, cy, size = detected
        cx += offset_x # Map back to full-screen coordinates
        
    # Step B: Get absolute precision from Landmark AI
    presence, landmarks, bbox = run_landmark_model(frame, cx, cy, size)
    
    if presence < PRESENCE_THRESHOLD:
        state["lost_count"] += 1
        return None
        
    # Step C: Update tracking box to tightly follow the hand next frame
    state["lost_count"] = 0
    xs, ys = [p[0] for p in landmarks], [p[1] for p in landmarks]
    new_size = max(int(max(max(xs)-min(xs), max(ys)-min(ys)) * 1.8), 60)
    state["box"] = ((min(xs)+max(xs))//2, (min(ys)+max(ys))//2, new_size)  
    
    # Draw green tracking box to visualize the "AI lock-on"
    cv2.rectangle(frame, (bbox[0], bbox[1]), (bbox[2], bbox[3]), (0, 255, 0), 2)
    return landmarks

# --- 5. MAIN LOOP ---
cap = cv2.VideoCapture(0)

while cap.isOpened():
    success, frame = cap.read()
    if not success: continue

    if calibration_start_time is None: calibration_start_time = time.time()

    frame = cv2.flip(frame, 1)
    h, w = frame.shape[:2]

    # Process hands globally with the Dual-AI system
    left_landmarks = process_hand_region(frame, "Left", w, h)
    right_landmarks = process_hand_region(frame, "Right", w, h)

    controls = {"move_x": 0.0, "move_y": 0.0, "aim_angle": 0.0, "shoot": 0}
    current_positions = {"Left": None, "Right": None}
    time_elapsed = time.time() - calibration_start_time

    # --- PROCESS LEFT HAND ---
    if left_landmarks:
        palm_x = (left_landmarks[0][0] + left_landmarks[9][0]) / 2.0
        palm_y = (left_landmarks[0][1] + left_landmarks[9][1]) / 2.0
        current_positions["Left"] = (palm_x, palm_y)
        for (lx, ly) in left_landmarks: cv2.circle(frame, (lx, ly), 4, (255, 0, 0), -1)

    # --- PROCESS RIGHT HAND ---
    if right_landmarks:
        wrist = right_landmarks[0]
        middle_knuckle = right_landmarks[9]
        middle_tip = right_landmarks[12]
        
        palm_x = (wrist[0] + middle_knuckle[0]) / 2.0
        palm_y = (wrist[1] + middle_knuckle[1]) / 2.0
        current_positions["Right"] = (palm_x, palm_y)
        
        hand_length = math.hypot(middle_knuckle[0] - wrist[0], middle_knuckle[1] - wrist[1])
        if time_elapsed > CALIBRATION_DURATION:
            fist_dist = math.hypot(middle_tip[0] - wrist[0], middle_tip[1] - wrist[1])
            if fist_dist < (hand_length * 1.2): 
                controls["shoot"] = 1
                
        for (rx, ry) in right_landmarks: cv2.circle(frame, (rx, ry), 4, (0, 0, 255), -1)

    # --- CALIBRATION STATE ---
    if time_elapsed <= CALIBRATION_DURATION:
        time_left = int(CALIBRATION_DURATION - time_elapsed) + 1
        cv2.putText(frame, f"CALIBRATING: Hold hands open! ({time_left}s)", (20, 50), 
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 255), 2)
        if current_positions["Left"]: centers["Left"] = current_positions["Left"]
        if current_positions["Right"]: centers["Right"] = current_positions["Right"]
        
    # --- ACTIVE STATE ---
    else:
        cv2.putText(frame, "DUAL AI ACTIVE: Full motion & fists supported", (20, 50), 
                    cv2.FONT_HERSHEY_SIMPLEX, 0.8, (0, 255, 0), 2)
        
        if centers["Left"] and current_positions["Left"]:
            dx = (current_positions["Left"][0] - centers["Left"][0]) / w 
            dy = (current_positions["Left"][1] - centers["Left"][1]) / h
            if abs(dx) > DEADZONE: controls["move_x"] = dx
            if abs(dy) > DEADZONE: controls["move_y"] = dy
            cv2.line(frame, (int(centers["Left"][0]), int(centers["Left"][1])), 
                            (int(current_positions["Left"][0]), int(current_positions["Left"][1])), (255, 0, 0), 2)

        if centers["Right"] and current_positions["Right"]:
            dx = current_positions["Right"][0] - centers["Right"][0]
            dy = current_positions["Right"][1] - centers["Right"][1]
            if math.hypot(dx/w, dy/h) > DEADZONE:
                controls["aim_angle"] = (math.degrees(math.atan2(dy, dx)) + 360) % 360
            cv2.line(frame, (int(centers["Right"][0]), int(centers["Right"][1])), 
                            (int(current_positions["Right"][0]), int(current_positions["Right"][1])), (0, 0, 255), 2)

        print(f"Move: ({controls['move_x']:.2f}, {controls['move_y']:.2f}) | Aim: {controls['aim_angle']:.0f}° | Shoot: {controls['shoot']}")

    if centers["Left"]: cv2.circle(frame, (int(centers["Left"][0]), int(centers["Left"][1])), 30, (255, 0, 0), 2)
    if centers["Right"]: cv2.circle(frame, (int(centers["Right"][0]), int(centers["Right"][1])), 30, (0, 0, 255), 2)

    cv2.imshow("Pure TFLite Pipeline", frame)
    if cv2.waitKey(1) & 0xFF == 27: break

cap.release()
cv2.destroyAllWindows()