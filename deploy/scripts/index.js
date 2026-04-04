
function isValidUrl(string) {
    // Regex for http:// or https:// followed by domain and path
    const regex = /^(https?:\/\/)[^\s$.?#].[^\s]*$/i;
    return regex.test(string);
}

const form = document.getElementById('submit-form');
form.addEventListener('submit', async function(event) {
    event.preventDefault(); // This stops the refresh
    const urlElement = document.getElementById('input-email')
    const errorElement = document.querySelector('.error-message');
    if (errorElement.classList.contains("error-present")) errorElement.classList.remove("error-present");
    if (urlElement.classList.contains("input-error")) urlElement.classList.remove("input-error");
    const url = urlElement.value.trim();
    if (!isValidUrl(url)) {
        console.error("Invalid URL:", url);
        errorElement.innerHTML = "Please enter a valid URL.";
        errorElement.classList.add("error-present");
        urlElement.classList.add("input-error");
        return
    }

    try {
        const response = await fetch('/api/shorten', {
            method: 'POST',
            body: JSON.stringify({ url }),
        });

        if (response.ok) {
            const result = await response.json();
            console.log("Success:", result);
            // Update UI here (e.g., show a success message)

            const inputResultElement = document.getElementById('input-result');
            inputResultElement.value = result.shortUrl;
        }
    } catch (error) {
        console.error("Error submitting form:", error);
    }
});

async function copyToClipboard() {
    const inputResultElement = document.getElementById('input-result');
    if (!inputResultElement.value) {
        return;
    }
    // inputResultElement.value = "random text to test copy functionality";
    await navigator?.clipboard?.writeText(inputResultElement.value);

    inputResultElement.setSelectionRange(0, 99999); // For mobile devices
}
