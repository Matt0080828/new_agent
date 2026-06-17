"""
Secret prompt utility for masked password input.
"""

import getpass


def masked_secret_prompt(prompt: str = "") -> str:
    """
    Prompt user for secret input (password) without echoing.
    
    Args:
        prompt: The prompt string to display
        
    Returns:
        The user's input as a string
    """
    return getpass.getpass(prompt)
