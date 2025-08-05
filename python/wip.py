from PIL import Image


def crop_equirectangular_map(image_path, left_lon, right_lon, top_lat, bottom_lat, output_path, new_size=(320, 240)):
    """
    Crop an Equirectangular projection map based on provided lat/lon bounds and resize it to the new size.

    :param image_path: Path to the full Equirectangular map image.
    :param left_lon: The leftmost longitude for cropping.
    :param right_lon: The rightmost longitude for cropping.
    :param top_lat: The topmost latitude for cropping.
    :param bottom_lat: The bottommost latitude for cropping.
    :param output_path: Path where the cropped and resized image will be saved.
    :param new_size: The desired size for the cropped image, default is (320, 240).
    :return: None
    """

    # Load the full Equirectangular map
    img = Image.open(image_path)

    # Image dimensions
    img_width, img_height = img.size

    # Map longitude (-180 to 180) to the image width (0 to img_width)
    lon_width_ratio = img_width / 360.0

    # Map latitude (-90 to 90) to the image height (0 to img_height)
    lat_height_ratio = img_height / 180.0

    # Calculate the pixel bounds for cropping
    left = int((left_lon + 180) * lon_width_ratio)
    right = int((right_lon + 180) * lon_width_ratio)
    top = int((90 - top_lat) * lat_height_ratio)
    bottom = int((90 - bottom_lat) * lat_height_ratio)

    # Crop the image
    cropped_img = img.crop((left, top, right, bottom))

    # Resize the cropped image
    cropped_img_resized = cropped_img.resize(new_size)

    # Save the cropped and resized image
    cropped_img_resized.save(output_path)

    print(f"Cropped and resized image saved to: {output_path}")
    cropped_img_resized.show()


# Function to get user input for cropping
def get_user_input_and_crop():
    #img = Image.open("map.png")

    # Ask for the path to the map image
    #image_path = input("Enter the path to the full Equirectangular map image (e.g., 'map.png'): ")

    left_lon = -12.71
    right_lon = 52.8
    bottom_lat = 34.8
    top_lat =71.7

    '''
    N: 71.7
    S : 34.8
    W: -12.71
    E: 52.8
    
    
    
    # Ask for cropping bounds (longitude and latitude)
    left_lon = float(input("Enter the leftmost longitude for cropping (e.g., -25 for 25°W): "))
    right_lon = float(input("Enter the rightmost longitude for cropping (e.g., 45 for 45°E): "))
    top_lat = float(input("Enter the topmost latitude for cropping (e.g., 71 for 71°N): "))
    bottom_lat = float(input("Enter the bottommost latitude for cropping (e.g., 36 for 36°N): "))

    # Ask for the output file path
    output_path = input("Enter the path to save the cropped and resized image (e.g., 'cropped_map.png'): ")
    '''
    output_path ="crop.png"
    # Call the cropping function with user inputs
    crop_equirectangular_map("map.png", left_lon, right_lon, top_lat, bottom_lat, output_path)


# Run the interactive input function
get_user_input_and_crop()
