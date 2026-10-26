#!/usr/bin/env bash
set -e

# ==============================================================================
# Grocycle Release Script
# Displays recent release tags, prompts for a new tag, and pushes to trigger
# GitHub Actions release workflow.
# ==============================================================================

CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

echo -e "${CYAN}${BOLD}======================================================${NC}"
echo -e "${CYAN}${BOLD}             Grocycle Release Helper                  ${NC}"
echo -e "${CYAN}${BOLD}======================================================${NC}"

# Ensure git repository
if ! git rev-parse --is-inside-work-tree &>/dev/null; then
    echo -e "${RED}Error: Must be run inside a Git repository.${NC}" >&2
    exit 1
fi

# Fetch remote tags to ensure local list is up-to-date
echo -e "\n${CYAN}Fetching latest tags from remote...${NC}"
git fetch --tags --quiet 2>/dev/null || true

# Display the top 5 most recent tags
echo -e "\n${YELLOW}${BOLD}Recent 5 tags:${NC}"
TAGS=$(git tag -l --sort=-v:refname | head -n 5)

if [ -z "$TAGS" ]; then
    echo -e "  (No tags found)"
else
    echo "$TAGS" | while read -r tag; do
        commit=$(git rev-parse --short "$tag" 2>/dev/null || echo "unknown")
        date=$(git log -1 --format="%cd" --date=short "$tag" 2>/dev/null || echo "")
        echo -e "  • ${GREEN}${tag}${NC} (${commit}) ${date}"
    done
fi

echo ""

# Prompt for the new tag
while true; do
    read -rp "Enter new release tag (e.g., v1.0.1): " NEW_TAG
    NEW_TAG=$(echo "$NEW_TAG" | tr -d '[:space:]')

    if [ -z "$NEW_TAG" ]; then
        echo -e "${RED}Tag cannot be empty. Please try again.${NC}"
        continue
    fi

    # Ensure semantic tag format matching v*.*.*
    if [[ ! "$NEW_TAG" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[a-zA-Z0-9.]+)?$ ]]; then
        echo -e "${RED}Invalid tag format. Tag must start with 'v' and follow semantic versioning (e.g., v1.0.1 or v1.0.1-rc1).${NC}"
        continue
    fi

    # Check if tag already exists
    if git rev-parse "$NEW_TAG" &>/dev/null; then
        echo -e "${RED}Tag '${NEW_TAG}' already exists! Please choose another tag name.${NC}"
        continue
    fi

    break
done

# Confirm action
echo -e "\nTarget tag: ${GREEN}${BOLD}${NEW_TAG}${NC}"
read -rp "Create and push tag '${NEW_TAG}' to origin? [y/N]: " CONFIRM
if [[ ! "$CONFIRM" =~ ^[Yy]$ ]]; then
    echo -e "${YELLOW}Release aborted.${NC}"
    exit 0
fi

# Create tag and push
echo -e "\n${CYAN}Creating tag '${NEW_TAG}'...${NC}"
git tag -a "$NEW_TAG" -m "Release $NEW_TAG"

echo -e "${CYAN}Pushing tag '${NEW_TAG}' to origin...${NC}"
git push origin "$NEW_TAG"

echo -e "\n${GREEN}${BOLD}✓ Tag '${NEW_TAG}' created and pushed successfully!${NC}"
echo -e "${GREEN}GitHub Actions will now build and publish the release packages (.deb, .tar.gz).${NC}"
echo -e "Track progress at: ${CYAN}https://github.com/$(git remote get-url origin 2>/dev/null | sed -E 's/.*[:\/]([^\/]+\/[^\/]+)(\.git)?$/\1/' | sed 's/\.git$//')/actions${NC}\n"
