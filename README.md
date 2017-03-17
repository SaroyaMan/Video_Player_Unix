=========================== Exercise 2 - UNIX System Programming ===========================

This application created by Yoav Saroya & Amit Shmuel (All rights and etc are reserved!)

Compile:
	Compile the project with 'make' command.
	
Clean:
	Clean the project and any other cruft with 'make clean' command.
	
How to run:
	The video application gets 4 arguments from the user: <primary video_file_name>  <second video_file_name> <width> <height>
	example: ./player f.mp4 s.mp4 1024 768
	another example: ./player f.mp4 s.mp4 (size of the screen will be default)
	
How to use:

	Audio control :
		'1' - Switch the audio channel to primary video file.
		'2' - Switch the audio channel to secondly video file.
		'3' - Mute the audio stream. Hit '3' again to play again the audio stream.
	
	Frame control:
		'w' - Remove all colors from frame and make it black & white.
		'r' - Color the frame to a red color.
		'g' - Color the frame to a green color.
		'b' - Color the frame to a blue color.
		'h' - Color the frame to a special YUV frame (Instant filter).
		'c' - Clear all customized colors and take it back to a 'normal' frame.
	
	Video control:
		'o' - Display only a single video - The one who streaming the audio.
		'm' - Display both 2 videos.
	
	Video Clock control:
		'left' - Go back 10 seconds to video who streaming the audio.
		'right' - Go forward 10 seconds to video who streaming the audio.
		'down' - Go back one minute to video who streaming the audio.
		'up' - Go forward one minute to video who streaming the audio.
		'f' - Fast forward (*2 FPS) to video who streaming the audio.
	
	General application control:
		'x' - Take a screenshot of the current video - The one who streaming the audio.
		'q' - Quit the video player application.
	
