variable "region" {
  description = "AWS region of the servers."
  type        = string
  default     = "us-east-1"
}

variable "az_suffix" {
  description = "Availability-zone letter for the subnet, instance and data volume (they must share one AZ)."
  type        = string
  default     = "a"
}

variable "domain" {
  description = "The domain (repository variable NAPPLE_DOMAIN). Its existing public hosted zone is looked up (by zone_id when set, else by this name), never created or destroyed by this stack."
  type        = string
  default     = "napple.lat"
}

variable "zone_id" {
  description = "Route 53 hosted zone id of the domain (repository variable NAPPLE_ZONE_ID). Empty = look the zone up by name (var.domain)."
  type        = string
  default     = ""
}

variable "server_subdomain" {
  description = "Host name of the game servers inside the zone (pretend -> pretend.napple.lat)."
  type        = string
  default     = "pretend"
}

variable "github_owner" {
  description = "GitHub user or organisation serving the landing page (<owner>.github.io is the CNAME target of www and the wildcard)."
  type        = string
  default     = "erostalevi"
}

variable "manage_pages_dns" {
  description = "Create the GitHub Pages records (apex A/AAAA, www CNAME, wildcard CNAME)."
  type        = bool
  default     = true
}

variable "wildcard_to_pages" {
  description = "Point *.<domain> at GitHub Pages. Only safe once the domain is verified in the GitHub account (Settings > Pages > Verified domains); see docs/guides/deploy-servers.md."
  type        = bool
  default     = true
}

variable "github_pages_verification_txt" {
  description = "Value of the _github-pages-challenge-<owner> TXT record GitHub shows when verifying the domain. Empty = no record."
  type        = string
  default     = ""
}

variable "dns_allow_overwrite" {
  description = "Let the stack take over records with the same name and type that already exist in the zone (made by hand in the console)."
  type        = bool
  default     = false
}

variable "dns_ttl" {
  description = "TTL in seconds of the records this stack creates."
  type        = number
  default     = 300
}

variable "instance_type" {
  description = "EC2 instance type. Graviton (t4g/m7g/c7g) needs architecture = arm64."
  type        = string
  default     = "t4g.small"
}

variable "architecture" {
  description = "CPU architecture of the AMI and of the container images the workflow builds."
  type        = string
  default     = "arm64"
  validation {
    condition     = contains(["arm64", "x86_64"], var.architecture)
    error_message = "architecture must be arm64 or x86_64."
  }
}

variable "root_volume_gb" {
  description = "Root volume size (OS, Docker images)."
  type        = number
  default     = 16
}

variable "data_volume_gb" {
  description = "Persistent volume mounted at /var/lib/napple (accounts, room state, TLS certificates). It survives instance replacement."
  type        = number
  default     = 10
}

variable "nex_udp_from" {
  description = "First UDP port of the NEX/PRUDP range opened to the internet."
  type        = number
  default     = 60000
}

variable "nex_udp_to" {
  description = "Last UDP port of the NEX/PRUDP range opened to the internet."
  type        = number
  default     = 60010
}

variable "room_port" {
  description = "napple-room port, opened for TCP and UDP."
  type        = number
  default     = 31415
}

variable "compose_version" {
  description = "Docker Compose plugin version installed by the instance user data (checked against the release's .sha256 file)."
  type        = string
  default     = "5.6.1"
}

variable "log_retention_days" {
  description = "CloudWatch retention of the container logs."
  type        = number
  default     = 30
}

variable "ecr_keep_images" {
  description = "Images kept per ECR repository (older ones expire; the rollback window)."
  type        = number
  default     = 30
}

variable "ecr_force_delete" {
  description = "Allow `tofu destroy` to delete the ECR repositories with images still in them."
  type        = bool
  default     = false
}

variable "vpc_cidr" {
  description = "CIDR of the stack's own VPC (one public subnet, no NAT)."
  type        = string
  default     = "10.42.0.0/16"
}
